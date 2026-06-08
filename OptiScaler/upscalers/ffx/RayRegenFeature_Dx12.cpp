#include <pch.h>
#include <cmath>
#include <Config.h>
#include <Util.h>
#include <NVNGX_Parameter.h>
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

    // Resolve the per-game RR conversion profile (Step 2: default profile from [RayRegen] ini).
    _profile = ResolveRRProfile();
    LOG_INFO("RR profile: {0}", _profile.name);

    ffxCreateContextDescDenoiser denoiserDesc = { 0 };
    denoiserDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER;
    denoiserDesc.version = FFX_DENOISER_VERSION;
    denoiserDesc.maxRenderSize = { _renderWidth, _renderHeight };
    denoiserDesc.mode = _profile.denoiserMode; // 1-signal (only mode reachable from NGX-RR today)
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

    LOG_INFO("FFX-MLD Ray Regen context created ({0}x{1}, mode {2})", _renderWidth, _renderHeight,
             _profile.denoiserMode);

    _convert = std::make_unique<RR_Dx12>("RayRegenConvert", Device);
    if (_convert == nullptr || !_convert->IsInit())
    {
        LOG_ERROR("Failed to create the NGX-RR -> MLD conversion shader");
        return false;
    }

    _resolve = std::make_unique<RR_Resolve_Dx12>("RayRegenResolve", Device);
    if (_resolve == nullptr || !_resolve->IsInit())
    {
        LOG_ERROR("Failed to create the RR resolve/recomposition shader");
        return false;
    }

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

    // Track whether the camera planes came from NGX or our fallback default: if Cyberpunk does not populate
    // FSR.cameraNear/Far we run on 0.1/10000, which mis-scales depth and over-blurs. Logged below (debug).
    bool nearFromNgx = (InParameters->Get(OptiKeys::FSR_NearPlane, &camNear) == NVSDK_NGX_Result_Success &&
                        camNear > 0.0f);
    if (!nearFromNgx)
        camNear = 0.1f;
    bool farFromNgx = (InParameters->Get(OptiKeys::FSR_FarPlane, &camFar) == NVSDK_NGX_Result_Success &&
                       camFar > 0.0f);
    if (!farFromNgx)
        camFar = 10000.0f;
    if (InParameters->Get(OptiKeys::FSR_CameraFovVertical, &camFov) != NVSDK_NGX_Result_Success || camFov <= 0.0f)
        camFov = 1.047198f; // 60 degrees in radians
    if (InParameters->Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &frameTimeMs) != NVSDK_NGX_Result_Success ||
        frameTimeMs < 1.0f)
        frameTimeMs = 16.7f;

    // Camera matrices (DLSS-RR contract): Cyberpunk passes WorldToView + ViewToClip; the projection encodes
    // the real near/far/fov. Derive the depth linearization coefficients + real near/far/fov from it (replaces
    // the guessed 0.1/10000). Row-major, left-multiply (NVIDIA convention): clip.z = vz*M[2][2] + M[3][2],
    // clip.w = vz*M[2][3] + M[3][3]  ->  viewZ = (B - dd*D) / (dd*C - A).
    float depthMatA = 0.0f, depthMatB = 0.0f, depthMatC = 0.0f, depthMatD = 0.0f;
    uint32_t hasDepthMatrix = 0;
    void* viewToClipPtr = nullptr;
    if (InParameters->Get("ViewToClipMatrix", &viewToClipPtr) == NVSDK_NGX_Result_Success && viewToClipPtr != nullptr)
    {
        const float* m = reinterpret_cast<const float*>(viewToClipPtr);
        depthMatA = m[10]; // M[2][2]
        depthMatB = m[14]; // M[3][2]
        depthMatC = m[11]; // M[2][3]
        depthMatD = m[15]; // M[3][3]
        hasDepthMatrix = 1;

        auto viewZ = [&](float dd)
        {
            float den = dd * depthMatC - depthMatA;
            return (depthMatB - dd * depthMatD) / ((std::fabs(den) > 1e-9f) ? den : 1e-9f);
        };
        float vz0 = std::fabs(viewZ(0.0f)); // device 0 = far plane (reversed-Z)
        float vz1 = std::fabs(viewZ(1.0f)); // device 1 = near plane (reversed-Z)
        float nearV = (vz0 < vz1) ? vz0 : vz1;
        float farV = (vz0 > vz1) ? vz0 : vz1;
        if (nearV < 1e-3f)
            nearV = 1e-3f;
        if (!(farV > nearV) || farV > 1.0e6f)
            farV = 1.0e6f; // guard infinite-far / NaN
        camNear = nearV;
        camFar = farV;

        float m11 = std::fabs(m[5]); // M[1][1] = 1/tan(fovY/2)
        if (m11 > 1e-6f)
            camFov = 2.0f * std::atan(1.0f / m11);
    }

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
    // Cyberpunk sets albedo + specular hit distance under the DLSS-RR eval keys, not the GBuffer.* ones
    // (confirmed via the NGX param dump). DLSS-RR requires diffuse+specular albedo; hit distance is optional.
    ID3D12Resource* inDiffAlb = nullptr; InParameters->Get("DLSS.Input.DiffuseAlbedo", &inDiffAlb);
    ID3D12Resource* inSpecAlb = nullptr; InParameters->Get("DLSS.Input.SpecularAlbedo", &inSpecAlb);
    ID3D12Resource* inSpecHit = nullptr; InParameters->Get("DLSSD.SpecularHitDistance", &inSpecHit);

    // Only colour/depth/output are mandatory. Missing optional GBuffer inputs degrade gracefully (Step 1):
    // their SRV is bound to a valid stand-in (colour) and the shader ignores it via InputMask, so an
    // unprofiled game keeps rendering (to tune) instead of bricking the feature.
    if (inColor == nullptr || inOutput == nullptr || inDepth == nullptr)
    {
        LOG_ERROR("Missing mandatory NGX Ray Reconstruction input (color/output/depth)");
        return false;
    }

    uint32_t inputMask = 0;
    if (inMv != nullptr)      inputMask |= RR_INPUT_HAS_MOTIONVECTORS;
    if (inNormals != nullptr) inputMask |= RR_INPUT_HAS_NORMALS;
    if (inDiffAlb != nullptr) inputMask |= RR_INPUT_HAS_DIFFUSE_ALBEDO;
    if (inSpecAlb != nullptr) inputMask |= RR_INPUT_HAS_SPECULAR_ALBEDO;
    if (inSpecHit != nullptr) inputMask |= RR_INPUT_HAS_SPEC_HITDIST;

    if ((inputMask & RR_INPUT_ALL) != RR_INPUT_ALL)
        LOG_WARN("RR: missing core input(s) (mask 0x{0:x}); using neutral defaults", inputMask);

    // Bind a valid stand-in for any missing optional input; the shader ignores it via the mask.
    if (inMv == nullptr)      inMv = inColor;
    if (inNormals == nullptr) inNormals = inColor;
    if (inDiffAlb == nullptr) inDiffAlb = inColor;
    if (inSpecAlb == nullptr) inSpecAlb = inColor;
    if (inSpecHit == nullptr) inSpecHit = inColor;

    // Lazily allocate the conversion output textures (render-res; heap props copied from color).
    if (!_convert->CanRender() && !_convert->CreateBufferResources(Device, inColor, _renderWidth, _renderHeight))
    {
        LOG_ERROR("Failed to allocate conversion output buffers");
        return false;
    }

    // Lazily allocate the resolve pass's denoised intermediate (the MLD denoiser writes here, then the
    // resolve pass recomposes the sky / debug-visualizes into the app output).
    if (!_resolve->CanRender() && !_resolve->CreateBufferResources(Device, inColor, _renderWidth, _renderHeight))
    {
        LOG_ERROR("Failed to allocate the resolve intermediate buffer");
        return false;
    }

    // Convert the NGX-RR buffers into the MLD 1-signal inputs.
    RRConstants rrc {};
    rrc.RenderWidth = _renderWidth;
    rrc.RenderHeight = _renderHeight;
    rrc.MotionScaleX = _profile.motionScaleX;
    rrc.MotionScaleY = _profile.motionScaleY;
    rrc.NearPlane = camNear;
    rrc.FarPlane = camFar;
    rrc.ReversedZ = _profile.reversedZ ? 1u : 0u;
    rrc.NormalsArePacked = _profile.normalsArePacked ? 1u : 0u;
    rrc.DemodulateRadiance = _profile.demodulateRadiance ? 1u : 0u;
    rrc.InputMask = inputMask;
    rrc.SkyThreshold = _profile.skyThreshold;
    rrc.DebugCapture = _profile.debugLog ? 1u : 0u;
    rrc.DepthMatA = depthMatA;
    rrc.DepthMatB = depthMatB;
    rrc.DepthMatC = depthMatC;
    rrc.DepthMatD = depthMatD;
    rrc.HasDepthMatrix = hasDepthMatrix;

    if (!_convert->Dispatch(InCommandList, rrc, inColor, inDepth, inMv, inNormals, inDiffAlb, inSpecAlb, inSpecHit))
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

    // Debug visualization (DebugView != 0) routes a converted signal straight to the output and skips
    // the denoiser, so we can ground-truth the conversion in-game. Normal path runs the denoiser into the
    // resolve pass's intermediate, then recomposes the sky from the skip-signal.
    const uint32_t debugView = _profile.debugView;

    if (debugView == 0)
    {
        // 1-signal radiance: noisy radiance in (from converter) -> denoised out (resolve intermediate).
        _resolve->PrepareForDenoiser(InCommandList);

        ffxDispatchDescDenoiserInput1Signal sig = { 0 };
        sig.header.type = FFX_API_DISPATCH_DESC_INPUT_1_SIGNAL_TYPE_DENOISER;
        sig.radiance.input = ffxApiGetResourceDX12(_convert->Radiance(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        sig.radiance.output = ffxApiGetResourceDX12(_resolve->Denoised(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        sig.fusedAlbedo = ffxApiGetResourceDX12(_convert->FusedAlbedo(), FFX_API_RESOURCE_STATE_COMPUTE_READ);

        dispatchDesc.header.pNext = &sig.header;

        auto ret = FfxApiProxy::D3D12_Dispatch(&_denoiserContext, &dispatchDesc.header);

        if (ret != FFX_API_RETURN_OK)
        {
            LOG_ERROR("D3D12_Dispatch (denoiser) error: {0}", FfxApiProxy::ReturnCodeToString(ret));
            return false;
        }
    }

    // Resolve: recompose the sky over the denoised result (DebugView 0), or write a debug view.
    RRResolveConstants rrr {};
    rrr.RenderWidth = _renderWidth;
    rrr.RenderHeight = _renderHeight;
    rrr.DebugView = debugView;
    rrr.NearPlane = camNear;
    rrr.FarPlane = camFar;

    if (!_resolve->Dispatch(InCommandList, rrr, _convert->SkipSignal(), _convert->Radiance(),
                            _convert->LinearDepth(), _convert->MotionVectors(), _convert->Normals(),
                            _convert->FusedAlbedo(), inOutput))
    {
        LOG_ERROR("RR resolve dispatch failed");
        return false;
    }

    // Periodic diagnostic logging (RrDebugLog). CPU-side context (the camera-plane source is the prime
    // over-blur suspect) + the GPU-sampled conversion values from the readback buffer.
    if (_profile.debugLog && (_frameCount % 120 == 0))
    {
        const char* nearSrc = hasDepthMatrix ? "mtx" : (nearFromNgx ? "ngx" : "default");
        const char* farSrc = hasDepthMatrix ? "mtx" : (farFromNgx ? "ngx" : "default");
        LOG_INFO("RR-debug ctx: frame={0} render={1}x{2} inputMask=0x{3:x} camNear={4:.4f}({5}) "
                 "camFar={6:.1f}({7}) fovV={8:.4f} debugView={9} skyThreshold={10:.4f} reversedZ={11} demod={12}",
                 _frameCount, _renderWidth, _renderHeight, inputMask, camNear, nearSrc, camFar, farSrc, camFov,
                 debugView, _profile.skyThreshold, _profile.reversedZ, _profile.demodulateRadiance);
        LOG_INFO("RR-debug depthMtx: has={0} A={1:.5f} B={2:.5f} C={3:.5f} D={4:.5f}", hasDepthMatrix, depthMatA,
                 depthMatB, depthMatC, depthMatD);
        _convert->LogDebugSamples();
    }

    // Full NGX parameter dump (coarser cadence). DLSS-RR's contract (NVIDIA vk_denoise_dlssrr) requires
    // diffuse+specular albedo and passes the camera as WorldToView + ViewToClip matrices (the projection
    // encodes the real near/far). We get inputMask=0x3 + default camera planes, so Cyberpunk sets those
    // under keys we do not read. OptiScaler's param object stores every key the game Set -> enumerate and
    // dump them all to find the matrix + albedo + hit-distance keys. Interpret by name + which probe hits:
    // res!=0 only = texture; res!=0 && ptr!=0 = CPU pointer (e.g. a matrix's float*); else scalar (ull/f).
    if (_profile.debugLog && (_frameCount % 600 == 0))
    {
        auto* p = static_cast<NVNGX_Parameters*>(InParameters);
        auto keys = p->enumerate();
        LOG_INFO("RR-debug NGX param dump (frame {0}, {1} keys):", _frameCount, keys.size());
        for (const auto& k : keys)
        {
            ID3D12Resource* res = nullptr;
            void* vp = nullptr;
            unsigned long long u = 0;
            float f = 0.0f;
            p->Get(k.c_str(), &res);
            p->Get(k.c_str(), &vp);
            p->Get(k.c_str(), &u);
            p->Get(k.c_str(), &f);
            LOG_INFO("  NGX[{0}] res={1} ptr={2} ull={3} f={4:.4f}", k, (void*) res, vp, u, f);
        }
    }

    _frameCount++;
    return true;
}
