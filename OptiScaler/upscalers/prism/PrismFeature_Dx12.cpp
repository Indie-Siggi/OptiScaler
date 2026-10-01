#include <pch.h>
#include <Config.h>
#include <Util.h>

#include "PrismFeature_Dx12.h"

#ifdef OPTISCALER_PRISM

#include <misc/IdentifyGpu.h>
#include "MathUtils.h"

#include <prism/nss.h>
#include <prism/prism.h>
#include <prism/prism_vulkan.h>

#include <wrl/client.h>

#include <cmath>
#include <ctime>
#include <cwchar>
#include <deque>
#include <filesystem>

using Microsoft::WRL::ComPtr;
using namespace OptiMath;

// vkd3d-proton's interop interface, version 1 (include/vkd3d_device_vkd3d_ext.idl); IdentifyGpu.h declares the base.
MIDL_INTERFACE("902d8115-59eb-4406-9518-fe00f991ee65")
ID3D12DXVKInteropDevice1 : public ID3D12DXVKInteropDevice
{
    virtual HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo1(ID3D12Resource * pResource, UINT64 * pVkHandle,
                                                             UINT64 * pBufferOffset, VkFormat * pFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateInteropCommandQueue(const D3D12_COMMAND_QUEUE_DESC* pDesc,
                                                                UINT32 vkQueueFamilyIndex,
                                                                ID3D12CommandQueue** ppQueue) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateInteropCommandAllocator(D3D12_COMMAND_LIST_TYPE type,
                                                                    UINT32 vkQueueFamilyIndex,
                                                                    ID3D12CommandAllocator** ppAllocator) = 0;
    virtual HRESULT STDMETHODCALLTYPE BeginVkCommandBufferInterop(ID3D12CommandList * pCmdList,
                                                                  VkCommandBuffer * pCommandBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndVkCommandBufferInterop(ID3D12CommandList * pCmdList) = 0;
};

struct PrismFeatureDx12::Impl
{
    ComPtr<ID3D12DXVKInteropDevice1> interop;
    ComPtr<ID3D12CommandQueue> queue; // only to learn the Vulkan queue family the game's direct lists record on
    std::unique_ptr<prism::Device> device;
    std::shared_ptr<const prism::Model> model;
    std::unique_ptr<prism::NssUpscaler> upscaler;
    // Replaced upscalers, destroyed a few evaluations later: frames they recorded may still be executing.
    std::deque<std::unique_ptr<prism::NssUpscaler>> retired;
    unsigned int width = 0, height = 0, outWidth = 0, outHeight = 0;
    unsigned int evaluationsSinceRetire = 0;
    bool exposureWarned = false;
    bool inputsLogged = false;
    bool maskLogged = false, maskUnsupported = false; // the bias current color mask's format, logged once
    std::string lastCaptureError; // logged once
};

namespace
{

// The game's resource as a Vulkan image region for Prism, in the layout vkd3d-proton uses for `state`.
bool ToNssImage(ID3D12DXVKInteropDevice1* interop, ID3D12Resource* resource, D3D12_RESOURCE_STATES state,
                bool output, prism::NssImage* image, const char* name)
{
    UINT64 handle = 0, offset = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;

    if (FAILED(interop->GetVulkanResourceInfo1(resource, &handle, &offset, &format)) || handle == 0)
    {
        LOG_ERROR("Prism: no Vulkan image for {}", name);
        return false;
    }

    if (!prism::nss_image_format_supported(format, output))
    {
        LOG_ERROR("Prism: {} has unsupported Vulkan format {}", name, (int) format);
        return false;
    }

    image->image = (VkImage) handle;
    image->format = format;
    interop->GetVulkanImageLayout(resource, state, &image->layout);
    return true;
}

ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* key)
{
    ID3D12Resource* resource = nullptr;

    if (params->Get(key, &resource) != NVSDK_NGX_Result_Success)
        params->Get(key, (void**) &resource);

    return resource;
}

} // namespace

PrismFeatureDx12::PrismFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters), _impl(std::make_unique<Impl>())
{
    _initParameters = SetInitParameters(InParameters); // the create flags and the render / output sizes
    _moduleLoaded = Config::Instance()->PrismModelPath.has_value();

    if (!_moduleLoaded)
        LOG_ERROR("Prism: [Prism] ModelPath is not set");
}

PrismFeatureDx12::~PrismFeatureDx12()
{
    // Prism does not wait for vkd3d-proton's device (prism_vulkan.h): like any NGX feature, this is released after
    // the game's GPU work using it has finished.
    _impl->upscaler.reset();
    _impl->retired.clear();
    _impl->model.reset();
    _impl->device.reset();
}

bool PrismFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (IsInited())
        return true;

    if (FAILED(Device->QueryInterface(IID_PPV_ARGS(&_impl->interop))))
    {
        LOG_ERROR("Prism: needs vkd3d-proton (no ID3D12DXVKInteropDevice1)");
        return false;
    }

    try
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        if (FAILED(Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_impl->queue))))
        {
            LOG_ERROR("Prism: CreateCommandQueue failed");
            return false;
        }

        prism::ExternalDevice ext;

        if (FAILED(_impl->interop->GetVulkanHandles(&ext.instance, &ext.physical, &ext.device)) ||
            FAILED(_impl->interop->GetVulkanQueueInfo(_impl->queue.Get(), &ext.queue, &ext.queue_family)))
        {
            LOG_ERROR("Prism: can't get vkd3d-proton's Vulkan handles");
            return false;
        }

        _impl->device = std::make_unique<prism::Device>(ext);
        _impl->model = prism::load_model(std::filesystem::path(Config::Instance()->PrismModelPath.value()));
        LOG_INFO("Prism: {} on {}", prism::info(*_impl->model).name, _impl->device->name());
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Prism: {}", e.what());
        return false;
    }

    SetInit(true);
    return true;
}

bool PrismFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    auto& cfg = *Config::Instance();

    if (!LowResMV())
    {
        LOG_ERROR("Prism: display-resolution motion vectors are not supported");
        return false;
    }

    unsigned int width = 0, height = 0;
    GetRenderResolution(InParameters, &width, &height);
    const unsigned int outWidth = TargetWidth(), outHeight = TargetHeight();

    ID3D12Resource* color = GetResource(InParameters, NVSDK_NGX_Parameter_Color);
    ID3D12Resource* motion = GetResource(InParameters, NVSDK_NGX_Parameter_MotionVectors);
    ID3D12Resource* depth = GetResource(InParameters, NVSDK_NGX_Parameter_Depth);
    ID3D12Resource* output = GetResource(InParameters, NVSDK_NGX_Parameter_Output);

    if (!color || !motion || !depth || !output)
    {
        LOG_ERROR("Prism: needs color, motion vectors, depth and output");
        return false;
    }

    if (!_impl->exposureWarned && GetResource(InParameters, NVSDK_NGX_Parameter_ExposureTexture))
    {
        LOG_WARN("Prism: the exposure texture is not used yet, [Prism] Exposure is");
        _impl->exposureWarned = true;
    }

    // The resources' states DLSS requires; the game (or OptiScaler's barrier options) puts them there.
    prism::NssFrameImages frame;
    auto* interop = _impl->interop.Get();

    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (!ToNssImage(interop, color, read, false, &frame.color, "color") ||
        !ToNssImage(interop, motion, read, false, &frame.motion, "motion") ||
        !ToNssImage(interop, depth, read, false, &frame.depth, "depth") ||
        !ToNssImage(interop, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true, &frame.output, "output"))
    {
        return false;
    }

    // The game's bias current color (reactive) mask: NSS has no input for it, a frame capture records it. Optional:
    // an unusable mask never fails the frame.
    if (auto* mask = GetResource(InParameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask);
        mask && !_impl->maskUnsupported)
    {
        if (!ToNssImage(interop, mask, read, false, &frame.mask, "bias current color mask"))
            _impl->maskUnsupported = true; // logged by ToNssImage, once
        else if (!_impl->maskLogged)
        {
            _impl->maskLogged = true;
            auto desc = mask->GetDesc();
            LOG_INFO("Prism: bias current color mask {}x{}, DXGI format {}, Vulkan format {}", desc.Width,
                     desc.Height, (int) desc.Format, (int) frame.mask.format);
        }
    }

    // NGX motion vectors times MV_Scale are pixels pointing to the previous position; NSS wants current minus previous.
    float mvScale[2] = { 1.0f, 1.0f };
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScale[0]);
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScale[1]);
    frame.motion_scale[0] = -mvScale[0] * cfg.PrismMotionScaleX.value_or_default();
    frame.motion_scale[1] = -mvScale[1] * cfg.PrismMotionScaleY.value_or_default();

    prism::NssFrameParams params;
    params.exposure = cfg.PrismExposure.value_or_default();
    params.history_clamp = cfg.PrismHistoryClamp.value_or_default();
    params.debug_view = cfg.PrismDebugView.value_or_default();
    params.anti_ghost = cfg.PrismAntiGhost.value_or_default();
    params.disocclusion_gate = cfg.PrismDisocclusionGate.value_or_default();
    params.disocclusion_tolerance = cfg.PrismDisocclusionTolerance.value_or_default();
    params.disocclusion_motion = cfg.PrismDisocclusionMotion.value_or_default();

    float jitter[2] = {};
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitter[0]);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitter[1]);
    // NSS wants where the sample sat relative to the pixel center, (row, column); NGX's jitter is the opposite
    // (Cyberpunk 2077: both axes flipped was sharp and stable, either unflipped blurred and shimmered).
    params.jitter[0] = -jitter[1] * cfg.PrismJitterScaleY.value_or_default();
    params.jitter[1] = -jitter[0] * cfg.PrismJitterScaleX.value_or_default();
    params.render_size[0] = (float) height;
    params.render_size[1] = (float) width;

    // Device depth -> view depth, view = [1] / (depth - [0]), from the camera the FSR backends use; [2], [3] scale
    // NDC to view space (tan of the half FOVs, horizontal then vertical, as in Arm's data).
    float cameraNear = cfg.FsrCameraNear.value_or_default();
    float cameraFar = cfg.FsrCameraFar.value_or_default();
    float verticalFov = GetRadiansFromDeg(cfg.FsrVerticalFov.value_or_default());

    if (cfg.FsrUseFsrInputValues.value_or_default())
    {
        InParameters->Get("FSR.cameraNear", &cameraNear);
        InParameters->Get("FSR.cameraFar", &cameraFar);
        InParameters->Get(OptiKeys::FSR_CameraFovVertical, &verticalFov);
    }

    // NSS assumes standard depth (smaller is nearer): reversed-Z depth is imported as 1 - depth, so the params
    // always describe standard depth.
    frame.depth_inverted = DepthInverted();
    params.depth_params[0] = cameraFar / (cameraFar - cameraNear);
    params.depth_params[1] = -cameraNear * cameraFar / (cameraFar - cameraNear);

    const float tanHalfVertical = std::tan(verticalFov * 0.5f);
    params.depth_params[2] = tanHalfVertical * (float) width / (float) height;
    params.depth_params[3] = tanHalfVertical;

    // Which optional masks the game passes (NSS has no input for them; FSR uses the bias mask as its reactive mask).
    auto hasResource = [&](const char* name)
    {
        ID3D12Resource* resource = nullptr;
        return InParameters->Get(name, (void**) &resource) == NVSDK_NGX_Result_Success && resource != nullptr;
    };
    _accessToReactiveMask = hasResource(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask);
    _hasTM = hasResource(NVSDK_NGX_Parameter_TransparencyMask);
    if (!_impl->inputsLogged)
    {
        _impl->inputsLogged = true;
        LOG_INFO("Prism: game masks: bias current color {}, transparency {}, animated texture {}, exposure texture {}",
                 _accessToReactiveMask, _hasTM, hasResource(NVSDK_NGX_Parameter_AnimatedTextureMask),
                 hasResource(NVSDK_NGX_Parameter_ExposureTexture));
    }

    unsigned int reset = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &reset);
    if (cfg.PrismDisableHistory.value_or_default())
        reset = 1;

    try
    {
        if (!_impl->upscaler || width != _impl->width || height != _impl->height || outWidth != _impl->outWidth ||
            outHeight != _impl->outHeight)
        {
            // A new size restarts the sequence. The old upscaler's frames may still run: keep it a while.
            LOG_INFO("Prism: NSS {}x{} -> {}x{}, MV scale {} {}, jitter {} {}, depth inverted {}", width, height,
                     outWidth, outHeight, mvScale[0], mvScale[1], jitter[0], jitter[1], DepthInverted());
            if (_impl->upscaler)
                _impl->retired.push_back(std::move(_impl->upscaler));
            _impl->evaluationsSinceRetire = 0;
            _impl->upscaler = std::make_unique<prism::NssUpscaler>(*_impl->device, _impl->model, (int) height,
                                                                   (int) width, (int) outHeight, (int) outWidth);
            _impl->width = width;
            _impl->height = height;
            _impl->outWidth = outWidth;
            _impl->outHeight = outHeight;
            reset = 1;
        }

        if (cfg.PrismCaptureNow.value_or_default())
        {
            cfg.PrismCaptureNow = false;
            std::filesystem::path base = cfg.PrismCaptureDir.has_value()
                                             ? std::filesystem::path(cfg.PrismCaptureDir.value())
                                             : Util::DllPath().parent_path() / "prism-capture";
            std::time_t now = std::time(nullptr);
            std::tm local {};
            localtime_s(&local, &now);
            wchar_t stamp[32];
            std::wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &local);
            std::filesystem::path dir = base / stamp;
            int frames = std::clamp(cfg.PrismCaptureFrames.value_or_default(), 1, 600);
            _impl->upscaler->start_capture(wstring_to_string(dir.wstring()), frames);
            cfg.PrismCaptureLastDir = dir.wstring();
            cfg.PrismCaptureWritten = 0;
            cfg.PrismCaptureRequested.store(_impl->upscaler->capture_requested(), std::memory_order_release);
            LOG_INFO("Prism: capturing {} frames into {}", frames, wstring_to_string(dir.wstring()));
        }

        if (FAILED(interop->BeginVkCommandBufferInterop(InCommandList, &frame.cmd)))
        {
            LOG_ERROR("Prism: BeginVkCommandBufferInterop failed");
            return false;
        }

        _impl->upscaler->record(frame, params, reset != 0);
        interop->EndVkCommandBufferInterop(InCommandList);

        if (cfg.PrismCaptureRequested.load() > 0)
        {
            int written = _impl->upscaler->capture_written();
            if (written != cfg.PrismCaptureWritten.load())
            {
                cfg.PrismCaptureWritten = written;
                if (written == _impl->upscaler->capture_requested())
                    LOG_INFO("Prism: capture done, {} frames", written);
            }
            cfg.PrismCaptureRequested = _impl->upscaler->capture_requested(); // shortened if it stopped early
            if (auto error = _impl->upscaler->capture_error(); error != _impl->lastCaptureError)
            {
                if (!error.empty())
                    LOG_ERROR("Prism: capture: {}", error);
                _impl->lastCaptureError = error;
            }
        }

        // Games keep a few frames in flight; eight evaluations after a resize the old upscaler is long done.
        if (!_impl->retired.empty() && ++_impl->evaluationsSinceRetire > 8)
        {
            _impl->retired.clear();
            _impl->evaluationsSinceRetire = 0;
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Prism: {}", e.what());
        return false;
    }

    _frameCount++; // OptiScaler treats a feature whose frame count stops as unused by the game

    return true;
}

#else // !OPTISCALER_PRISM

struct PrismFeatureDx12::Impl
{
};

PrismFeatureDx12::PrismFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters)
{
    _moduleLoaded = false;
    LOG_ERROR("Prism: this OptiScaler was built without Prism");
}

PrismFeatureDx12::~PrismFeatureDx12() = default;

bool PrismFeatureDx12::InitInternal(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*) { return false; }
bool PrismFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*) { return false; }

#endif
