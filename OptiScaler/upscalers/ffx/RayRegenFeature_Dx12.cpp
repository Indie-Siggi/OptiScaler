#include <pch.h>
#include <Config.h>
#include <Util.h>
#include <proxies/FfxApi_Proxy.h>
#include "RayRegenFeature_Dx12.h"

// AMD FSR Ray Regeneration (FFX-MLD) backend. See RayRegenFeature_Dx12.h and
// OPTISCALER_RR_PLAN.md "Path B" for the NGX-RR -> MLD 1-signal mapping.
//
// SCAFFOLD STATE (Phase 1): the denoiser context is created and dispatched for real, but the
// NGX-RR inputs are passed through raw rather than converted into MLD encodings. The conversion
// shader (Phase 2) must still: linearize depth, octahedral-encode normals (+roughness/material),
// sqrt-encode albedo, build fusedAlbedo = sqrt(max(spec,diff)), pack UV motion vectors + depth
// delta, and demodulate the radiance. Until then the visual result is not correct, but the path
// compiles, links, creates the context, and dispatches end to end.

RayRegenFeatureDx12::RayRegenFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : IFeature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters)
{
    FfxApiProxy::InitFfxDx12();

    // The denoiser ships in the same amd_fidelityfx_dx12.dll as the upscaler, so the SR-ready
    // check doubles as the "FFX API methods are loaded" check for the MLD backend.
    _moduleLoaded = FfxApiProxy::IsSRReady();

    if (_moduleLoaded)
        LOG_INFO("amd_fidelityfx_dx12.dll methods loaded (FFX-MLD Ray Regen backend)!");
    else
        LOG_ERROR("can't load amd_fidelityfx_dx12.dll methods!");
}

RayRegenFeatureDx12::~RayRegenFeatureDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    ReleaseDenoiserContext();
}

void RayRegenFeatureDx12::ReleaseDenoiserContext()
{
    if (_denoiserContext != nullptr)
    {
        FfxApiProxy::D3D12_DestroyContext(&_denoiserContext, nullptr);
        _denoiserContext = nullptr;
    }
}

bool RayRegenFeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LOG_DEBUG("RayRegenFeatureDx12::Init");

    if (IsInited())
        return true;

    return CreateDenoiserContext(InCommandList, InParameters);
}

bool RayRegenFeatureDx12::CreateDenoiserContext(ID3D12GraphicsCommandList* InCommandList,
                                                NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (!ModuleLoaded())
        return false;

    if (Device == nullptr)
    {
        LOG_ERROR("D3D12Device is null!");
        return false;
    }

    GetRenderResolution(InParameters, &_renderWidth, &_renderHeight);

    ffxCreateContextDescDenoiser denoiserDesc = { 0 };
    denoiserDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    denoiserDesc.version = FFX_DENOISER_VERSION;
    denoiserDesc.maxRenderSize = { _renderWidth, _renderHeight };
    denoiserDesc.mode = _mode; // FFX_DENOISER_MODE_1_SIGNAL (only mode reachable from NGX-RR)
    denoiserDesc.flags = 0;

    ffxCreateBackendDX12Desc backendDesc = { 0 };
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.device = Device;

    denoiserDesc.header.pNext = &backendDesc.header;

    {
        ScopedSkipHeapCapture skipHeapCapture {};

        auto ret = FfxApiProxy::D3D12_CreateContext(&_denoiserContext, &denoiserDesc.header, nullptr);

        if (ret != FFX_API_RETURN_OK)
        {
            LOG_ERROR("D3D12_CreateContext (denoiser) error: {0}", FfxApiProxy::ReturnCodeToString(ret));
            return false;
        }
    }

    LOG_INFO("FFX-MLD Ray Regen context created ({0}x{1}, mode {2})", _renderWidth, _renderHeight, _mode);

    _convert = std::make_unique<RR_Dx12>("RayRegenConvert", Device);
    if (_convert == nullptr || !_convert->IsInit())
    {
        LOG_ERROR("Failed to create the NGX-RR -> MLD conversion shader");
        return false;
    }

    // Load per-game conversion tunables from OptiScaler.ini [RayRegen]; tune in-game (Phase 5).
    const Config& cfg = *Config::Instance();
    _depthLinA = cfg.RrDepthLinA.value_or_default();
    _depthLinB = cfg.RrDepthLinB.value_or_default();
    _motionScaleX = cfg.RrMotionScaleX.value_or_default();
    _motionScaleY = cfg.RrMotionScaleY.value_or_default();
    _normalsArePacked = cfg.RrNormalsArePacked.value_or_default() ? 1u : 0u;
    _demodulateRadiance = cfg.RrDemodulateRadiance.value_or_default() ? 1u : 0u;

    _resetHistory = true;
    SetInit(true);
    return true;
}

bool RayRegenFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList,
                                           NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (_denoiserContext == nullptr)
    {
        LOG_ERROR("denoiser context is null!");
        return false;
    }

    ffxDispatchDescDenoiser dispatchDesc = { 0 };
    dispatchDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_DENOISER;
    dispatchDesc.commandList = InCommandList;

    GetRenderResolution(InParameters, &_renderWidth, &_renderHeight);
    dispatchDesc.renderSize = { _renderWidth, _renderHeight };

    // Motion vectors are expressed in UV space (PreviousUV - CurrentUV); B scales the depth delta.
    dispatchDesc.motionVectorScale = { 1.0f, 1.0f, 1.0f };

    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &dispatchDesc.jitterOffsets.x);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &dispatchDesc.jitterOffsets.y);

    unsigned int reset = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &reset);
    dispatchDesc.flags = (reset == 1 || _resetHistory) ? FFX_DENOISER_DISPATCH_RESET : 0;
    _resetHistory = false;

    dispatchDesc.frameIndex = static_cast<uint32_t>(_frameCount);

    // Camera near/far/fov + frame time. Read the FSR.* keys the game/OptiScaler may populate, else fall
    // back to sane defaults. The camera basis vectors (right/up/forward) and positionDelta require the
    // view matrix, which NGX does not expose here (only Position_ViewSpace), so they are left zero and the
    // denoiser falls back to motion-vector-only reprojection. TODO(Phase 5): recover the view matrix.
    float camNear = 0.0f, camFar = 0.0f, camFov = 0.0f, frameTimeMs = 0.0f;

    if (InParameters->Get(OptiKeys::FSR_NearPlane, &camNear) != NVSDK_NGX_Result_Success || camNear <= 0.0f)
        camNear = 0.1f;
    if (InParameters->Get(OptiKeys::FSR_FarPlane, &camFar) != NVSDK_NGX_Result_Success || camFar <= 0.0f)
        camFar = 10000.0f;
    if (InParameters->Get(OptiKeys::FSR_CameraFovVertical, &camFov) != NVSDK_NGX_Result_Success || camFov <= 0.0f)
        camFov = 1.047198f; // 60 degrees in radians
    if (InParameters->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &frameTimeMs) != NVSDK_NGX_Result_Success ||
        frameTimeMs < 1.0f)
        frameTimeMs = 16.7f;

    dispatchDesc.cameraNear = camNear;
    dispatchDesc.cameraFar = camFar;
    dispatchDesc.cameraFovAngleVertical = camFov;
    dispatchDesc.cameraAspectRatio = (_renderHeight > 0) ? (float) _renderWidth / (float) _renderHeight : 1.0f;
    dispatchDesc.deltaTime = frameTimeMs;

    // --- Read the intercepted NGX Ray-Reconstruction inputs --------------------------------------
    ID3D12Resource* inColor = nullptr;   InParameters->Get(NVSDK_NGX_Parameter_Color, &inColor);
    ID3D12Resource* inOutput = nullptr;  InParameters->Get(NVSDK_NGX_Parameter_Output, &inOutput);
    ID3D12Resource* inDepth = nullptr;   InParameters->Get(NVSDK_NGX_Parameter_Depth, &inDepth);
    ID3D12Resource* inMv = nullptr;      InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &inMv);
    ID3D12Resource* inNormals = nullptr; InParameters->Get(NVSDK_NGX_Parameter_GBuffer_Normals, &inNormals);
    ID3D12Resource* inDiffAlb = nullptr; InParameters->Get(NVSDK_NGX_Parameter_GBuffer_DiffuseAlbedo, &inDiffAlb);
    ID3D12Resource* inSpecAlb = nullptr; InParameters->Get(NVSDK_NGX_Parameter_GBuffer_SpecularAlbedo, &inSpecAlb);

    if (inColor == nullptr || inOutput == nullptr || inDepth == nullptr || inMv == nullptr ||
        inNormals == nullptr || inDiffAlb == nullptr || inSpecAlb == nullptr)
    {
        LOG_ERROR("Missing NGX Ray Reconstruction input(s) (color/output/depth/mv/normals/albedo)");
        return false;
    }

    // Lazily allocate the conversion output textures (render-res; heap props copied from color).
    if (!_convert->CanRender() && !_convert->CreateBufferResources(Device, inColor, _renderWidth, _renderHeight))
    {
        LOG_ERROR("Failed to allocate conversion output buffers");
        return false;
    }

    // Convert the NGX-RR buffers into the MLD 1-signal inputs.
    RRConstants rrc {};
    rrc.RenderWidth = _renderWidth;
    rrc.RenderHeight = _renderHeight;
    rrc.MotionScaleX = _motionScaleX;
    rrc.MotionScaleY = _motionScaleY;
    rrc.DepthLinA = _depthLinA;
    rrc.DepthLinB = _depthLinB;
    rrc.NormalsArePacked = _normalsArePacked;
    rrc.DemodulateRadiance = _demodulateRadiance;

    if (!_convert->Dispatch(InCommandList, rrc, inColor, inDepth, inMv, inNormals, inDiffAlb, inSpecAlb))
    {
        LOG_ERROR("NGX-RR -> MLD conversion dispatch failed");
        return false;
    }

    // Make the converted inputs readable by the denoiser.
    _convert->TransitionOutputs(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Albedo is passed linear, so tell MLD not to assume sqrt encoding.
    dispatchDesc.flags |= FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO;

    // Always-bound guide buffers (from the converter).
    dispatchDesc.linearDepth = ffxApiGetResourceDX12(_convert->LinearDepth(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.motionVectors = ffxApiGetResourceDX12(_convert->MotionVectors(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.normals = ffxApiGetResourceDX12(_convert->Normals(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.specularAlbedo = ffxApiGetResourceDX12(_convert->SpecularAlbedo(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.diffuseAlbedo = ffxApiGetResourceDX12(_convert->DiffuseAlbedo(), FFX_API_RESOURCE_STATE_COMPUTE_READ);

    // 1-signal radiance: noisy radiance in (from converter) -> denoised out (app output target).
    ffxDispatchDescDenoiserInput1Signal sig = { 0 };
    sig.header.type = FFX_API_DISPATCH_DESC_INPUT_1_SIGNAL_TYPE_DENOISER;
    sig.radiance.input = ffxApiGetResourceDX12(_convert->Radiance(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    sig.radiance.output = ffxApiGetResourceDX12(inOutput, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    sig.fusedAlbedo = ffxApiGetResourceDX12(_convert->FusedAlbedo(), FFX_API_RESOURCE_STATE_COMPUTE_READ);

    dispatchDesc.header.pNext = &sig.header;

    auto ret = FfxApiProxy::D3D12_Dispatch(&_denoiserContext, &dispatchDesc.header);

    if (ret != FFX_API_RETURN_OK)
    {
        LOG_ERROR("D3D12_Dispatch (denoiser) error: {0}", FfxApiProxy::ReturnCodeToString(ret));
        return false;
    }

    _frameCount++;
    return true;
}
