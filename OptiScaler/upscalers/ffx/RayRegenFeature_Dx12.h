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
#include <shaders/rr_convert/RR_Resolve_Dx12.h>
#include "RRGameProfile.h"
#include <memory>

class RayRegenFeatureDx12 : public IFeature_Dx12
{
  private:
    ffxContext _denoiserContext = nullptr;

    // Per-game RR conversion profile (input conventions + denoiser mode + tunables). Step 2: the default
    // profile from ResolveRRProfile(); per-game selection comes later (OPTISCALER_RR_PLAN.md Appendix B).
    RRGameProfile _profile;

    // Needed for ffxDispatchDescDenoiser.cameraPositionDelta (PreviousPosition - CurrentPosition).
    float _prevCameraPosition[3] = { 0.0f, 0.0f, 0.0f };
    bool _prevCamPosValid = false;
    bool _resetHistory = true;

    feature_version _version = { FFX_DENOISER_VERSION_MAJOR, FFX_DENOISER_VERSION_MINOR,
                                 FFX_DENOISER_VERSION_PATCH };

    // NGX-RR -> MLD input conversion (linearize depth, octahedral normals, fused albedo, UV
    // motion vectors, radiance, sky skip-signal). Produces the 8 MLD dispatch inputs. See shaders/rr_convert.
    std::unique_ptr<RR_Dx12> _convert;

    // Post-denoise resolve: recompose the sky from the skip-signal (the denoiser zeroes far pixels),
    // and/or write a debug visualization of a converted signal. Owns the denoised intermediate the
    // MLD denoiser writes into. See shaders/rr_convert/RR_Resolve_Dx12.
    std::unique_ptr<RR_Resolve_Dx12> _resolve;

    bool CreateDenoiserContext(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);
    void ReleaseDenoiserContext();

    // Push the [RayRegen] denoiser tuning floats (from _profile) into the denoiser once, at context
    // creation. Live per-frame ffxConfigure wedged the gfx ring, so changes apply only on RR restart.
    void ApplyDenoiserTuning();

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
