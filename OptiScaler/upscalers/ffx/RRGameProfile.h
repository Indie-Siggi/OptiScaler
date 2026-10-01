#pragma once

#include <cstdint>

// Per-game RR conversion profile.
//
// Holds the input conventions, tunables and denoiser mode as one "default" profile, so RayRegenFeature_Dx12
// routes through one struct. Games differ in how they pass the DLSS-RR inputs (normal space, motion vector
// convention, reversed-Z, albedo encoding, radiance scale); the plan is per-game profiles selected by
// State::GameName / NVNGX_Engine, with the [RayRegen] ini keys overriding any field.
struct RRGameProfile
{
    const char* name = "default";

    uint32_t denoiserMode = 2; // FfxApiDenoiserMode; 2 = FFX_DENOISER_MODE_1_SIGNAL

    // Conversion tunables (see shaders/rr_convert/RR_Common.h).
    float depthLinA = 1.0f;    // linearDepth = 1 / (A * deviceDepth + B)
    float depthLinB = 0.0f;
    float motionScaleX = 1.0f; // NGX motion vectors -> UV (PreviousUV - CurrentUV)
    float motionScaleY = 1.0f;
    bool normalsArePacked = true;
    bool demodulateRadiance = true; // demod radiance by fused albedo; the resolve pass re-modulates (paired)
    bool reversedZ = true; // device depth is reversed-Z (1=near, 0=far); Cyberpunk and most modern engines
    float skyThreshold = 0.999f; // viewZ >= FarPlane * this marks a sky pixel for the skip-signal bypass
    uint32_t debugView = 0; // 0 = normal (recompose); 1..8 = visualize a converted signal (see RR_Resolve_Common.h)
    bool debugLog = false;  // sample + log converted values (centre + sky pixel) periodically to OptiScaler.log
    bool reprojection = true; // feed world-space camera basis + positionDelta from WorldToViewMatrix to the denoiser
    // FFX-MLD denoiser tuning floats (ffxConfigure at context creation). -1 = no override (denoiser default).
    float crossBilateralNormalStrength = -1.0f;
    float stabilityBias = -1.0f;
    float maxRadiance = -1.0f;
    float radianceClipStdK = -1.0f;
    float gaussianKernelRelaxation = -1.0f;
    float disocclusionThreshold = -1.0f;
};

// Resolve the active RR profile: for now the default profile, populated from OptiScaler.ini [RayRegen].
// TODO: branch on State::Instance().GameName / NVNGX_Engine for per-game profiles, then apply
// the [RayRegen] ini overrides on top of the selected profile.
RRGameProfile ResolveRRProfile();
