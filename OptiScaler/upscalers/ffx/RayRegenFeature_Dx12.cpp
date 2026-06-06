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

    // TODO(Phase 3): deltaTime + camera vectors (positionDelta/right/up/forward, near/far/fov/aspect)
    // derived from the NGX view/projection matrices. Zeroed in the scaffold.

    // --- Guide buffers ---------------------------------------------------------------------------
    // Scaffold: raw NGX buffers are passed straight through. The conversion shader (Phase 2) must
    // replace these with MLD-encoded equivalents (abs-linear depth; octahedral normals + roughness +
    // material; sqrt albedo; UV motion vectors + depth delta).
    ID3D12Resource* paramColor = nullptr;
    InParameters->Get(NVSDK_NGX_Parameter_Color, &paramColor);

    ID3D12Resource* paramOutput = nullptr;
    InParameters->Get(NVSDK_NGX_Parameter_Output, &paramOutput);

    ID3D12Resource* paramDepth = nullptr;
    InParameters->Get(NVSDK_NGX_Parameter_Depth, &paramDepth);

    ID3D12Resource* paramMv = nullptr;
    InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &paramMv);

    if (paramColor == nullptr || paramOutput == nullptr)
    {
        LOG_ERROR("Color/Output resources not provided!");
        return false;
    }

    if (paramDepth != nullptr)
        dispatchDesc.linearDepth = ffxApiGetResourceDX12(paramDepth, FFX_API_RESOURCE_STATE_COMPUTE_READ);

    if (paramMv != nullptr)
        dispatchDesc.motionVectors = ffxApiGetResourceDX12(paramMv, FFX_API_RESOURCE_STATE_COMPUTE_READ);

    // TODO(Phase 2): dispatchDesc.normals / specularAlbedo / diffuseAlbedo from the NGX GBuffer,
    // in MLD encodings (octahedral normals; sqrt albedo). These are mandatory inputs for the MLD
    // dispatch, so the result is incomplete until the conversion shader populates them.

    // --- 1-signal radiance payload (chained via header.pNext) ------------------------------------
    ffxDispatchDescDenoiserInput1Signal sig = { 0 };
    sig.header.type = FFX_API_DISPATCH_DESC_INPUT_1_SIGNAL_TYPE_DENOISER;
    sig.radiance.input = ffxApiGetResourceDX12(paramColor, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    sig.radiance.output = ffxApiGetResourceDX12(paramOutput, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // TODO(Phase 2): sig.fusedAlbedo = sqrt(max(specularAlbedo, diffuseAlbedo)) from conversion shader.

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
