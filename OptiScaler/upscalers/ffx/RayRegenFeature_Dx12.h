#pragma once

// AMD FSR Ray Regeneration (FFX-MLD) backend for OptiScaler.
//
// Occupies the Upscaler::DLSSD (Ray Reconstruction) slot on RDNA4, where the NVIDIA
// DLSS-D backend is unavailable. The intercepted NGX Ray-Reconstruction inputs (noisy
// colour, device depth, motion vectors, normals, diffuse/specular albedo, camera matrices)
// are converted into the MLD 1-signal contract (radiance + fusedAlbedo, octahedral normals,
// sqrt-encoded albedo, linear depth, UV motion vectors) by a compute shader, then handed to
// AMD's denoiser via the FFX API (ffxCreateContextDescDenoiser / ffxDispatchDescDenoiser).
//
// Design + descriptor mapping: OPTISCALER_RR_PLAN.md "Path B" + "Appendix A".
// MLD contract: ffx_denoiser.h (vendored from FFX SDK 2.2, MIT).

#include "ffx_denoiser.h" // vendored: ffxCreateContextDescDenoiser / ffxDispatchDescDenoiserInput1Signal (C API)
#include <upscalers/IFeature_Dx12.h>
#include <shaders/rr_convert/RR_Dx12.h>
#include <memory>

class RayRegenFeatureDx12 : public IFeature_Dx12
{
  private:
    ffxContext _denoiserContext = nullptr;

    // 1-signal is the only mode reachable from the NGX-RR contract (a single combined noisy
    // colour). 2/4-signal need decomposed radiance the call site does not expose. See Appendix A.
    uint32_t _mode = FFX_DENOISER_MODE_1_SIGNAL;

    // Needed for ffxDispatchDescDenoiser.cameraPositionDelta (PreviousPosition - CurrentPosition).
    float _prevCameraPosition[3] = { 0.0f, 0.0f, 0.0f };
    bool _resetHistory = true;

    feature_version _version = { FFX_DENOISER_VERSION_MAJOR, FFX_DENOISER_VERSION_MINOR,
                                 FFX_DENOISER_VERSION_PATCH };

    // NGX-RR -> MLD input conversion (linearize depth, octahedral normals, fused albedo, UV
    // motion vectors, radiance). Produces the 7 MLD dispatch inputs. See shaders/rr_convert.
    std::unique_ptr<RR_Dx12> _convert;

    // Per-game conversion tunables (defaults; refine via the menu / in-game in Phase 5).
    float _depthLinA = 1.0f;          // linearDepth = 1 / (A * deviceDepth + B); TODO derive from NGX matrices
    float _depthLinB = 0.0f;
    float _motionScaleX = 1.0f;       // NGX motion vectors -> UV; TODO confirm per-game scale/sign
    float _motionScaleY = 1.0f;
    uint32_t _normalsArePacked = 1;   // assume normals stored as n*0.5+0.5; TODO confirm
    uint32_t _demodulateRadiance = 0; // divide noisy colour by fused albedo; TODO tune per-game

    bool CreateDenoiserContext(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);
    void ReleaseDenoiserContext();

  protected:
    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

  public:
    feature_version Version() override { return _version; }

    // Reuses the DLSSD slot (created via Upscaler::DLSSD); config/menu treat it as the RR backend.
    Upscaler GetUpscalerType() const final { return Upscaler::DLSSD; }

    bool IsWithDx12() override { return false; }

    RayRegenFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~RayRegenFeatureDx12();
};
