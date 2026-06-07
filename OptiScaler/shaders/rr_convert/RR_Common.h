#pragma once

#include "SysUtils.h"

// InputMask bits: which optional GBuffer inputs the game provided. Missing inputs are bound to a valid
// stand-in resource and ignored via the mask (neutral defaults). Keep in sync with shaderCode below.
#define RR_INPUT_HAS_MOTIONVECTORS   1u
#define RR_INPUT_HAS_NORMALS         2u
#define RR_INPUT_HAS_DIFFUSE_ALBEDO  4u
#define RR_INPUT_HAS_SPECULAR_ALBEDO 8u
#define RR_INPUT_ALL                 15u

// Constants for the NGX Ray-Reconstruction -> FFX-MLD input conversion shader.
// Field order/layout must match the cbuffer in shaderCode below.
struct alignas(256) RRConstants
{
    uint32_t RenderWidth;
    uint32_t RenderHeight;
    float MotionScaleX;       // multiply InMotionVectors.x to get UV-space (PreviousUV - CurrentUV)
    float MotionScaleY;       // multiply InMotionVectors.y
    float NearPlane;          // camera near (world units)
    float FarPlane;           // camera far  (world units)
    uint32_t NormalsArePacked; // 1 if normals are stored as n * 0.5 + 0.5
    uint32_t DemodulateRadiance; // 1 to divide the noisy colour by fused albedo (per-game; tune in-game)
    uint32_t InputMask;          // RR_INPUT_HAS_* bits; missing inputs use neutral defaults
    uint32_t ReversedZ;          // 1 if device depth is reversed-Z (1=near, 0=far), e.g. Cyberpunk
};

// HLSL converter. Runtime-compiled as cs_5_0 when UsePrecompiledShaders=false; otherwise the
// precompiled RR_cso (precompiled/RR_Shader.h, generated via shader_tools/dxc) is used.
//
// Produces the MLD 1-signal inputs from the intercepted DLSS-RR buffers:
//   linearDepth (abs linear), motionVectors (RG=UV, B=depth delta), normals (RG=octahedral,
//   B=linear roughness, A=material), specular/diffuse albedo (linear; dispatch sets
//   FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO), fusedAlbedo = max(spec,diff), radiance (noisy colour).
inline static std::string shaderCode = R"(
cbuffer Params : register(b0)
{
    uint  RenderWidth;
    uint  RenderHeight;
    float MotionScaleX;
    float MotionScaleY;
    float NearPlane;
    float FarPlane;
    uint  NormalsArePacked;
    uint  DemodulateRadiance;
    uint  InputMask;          // bit0 MV, bit1 normals, bit2 diffuseAlbedo, bit3 specularAlbedo
    uint  ReversedZ;          // 1 if device depth is reversed-Z (1=near, 0=far)
};

Texture2D<float4> InColor           : register(t0);
Texture2D<float>  InDepth           : register(t1);
Texture2D<float4> InMotionVectors   : register(t2);
Texture2D<float4> InNormalRoughness : register(t3); // RGB normal, A linear roughness
Texture2D<float4> InDiffuseAlbedo   : register(t4);
Texture2D<float4> InSpecularAlbedo  : register(t5);

RWTexture2D<float4> OutRadiance       : register(u0); // RGB noisy radiance, A specular ray length
RWTexture2D<float4> OutFusedAlbedo    : register(u1); // RGB max(spec,diff)
RWTexture2D<float>  OutLinearDepth    : register(u2); // R abs linear depth
RWTexture2D<float4> OutMotionVectors  : register(u3); // RG UV motion, B depth delta
RWTexture2D<float4> OutNormals        : register(u4); // RG octahedral, B roughness, A material
RWTexture2D<float4> OutSpecularAlbedo : register(u5);
RWTexture2D<float4> OutDiffuseAlbedo  : register(u6);

float2 SignNotZero(float2 v)
{
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// Octahedral encode a unit normal to [-1,1] in 2 channels.
float2 OctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z) + 1e-8);
    float2 e = n.xy;
    if (n.z < 0.0)
        e = (1.0 - abs(e.yx)) * SignNotZero(e.xy);
    return e;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= RenderWidth || tid.y >= RenderHeight)
        return;

    int3 p = int3(tid.xy, 0);

    // Device depth -> linear view-space depth (world units), clamped to [near, far].
    // Convention-aware closed form (handles reversed-Z) and finite at the far plane (sky), so the sky no
    // longer maps to 1/0 = Inf -> black. Reproduces the DarkHelmet oracle's clamp(viewZ, near, far) without
    // needing the projection matrix. See RR_BUILD_DEPLOY.md "Oracle conventions".
    float dd = InDepth.Load(p);
    float depthSpan = FarPlane - NearPlane;
    float denom = (ReversedZ != 0u) ? (NearPlane + dd * depthSpan) : (FarPlane - dd * depthSpan);
    float viewZ = (NearPlane * FarPlane) / max(denom, 1e-6);
    OutLinearDepth[tid.xy] = clamp(viewZ, NearPlane, FarPlane);

    // Motion vectors -> UV space (PreviousUV - CurrentUV). B = depth delta (needs history; 0 for now).
    // Missing (bit0 clear) -> zero motion (denoiser treats the pixel as static).
    float2 mv = (InputMask & 1u) ? InMotionVectors.Load(p).xy : float2(0.0, 0.0);
    OutMotionVectors[tid.xy] = float4(mv.x * MotionScaleX, mv.y * MotionScaleY, 0.0, 0.0);

    // Normal (+ roughness) -> octahedral RG, B roughness, A material type (0 = default).
    // Missing (bit1 clear) -> facing normal (0,0,1) + mid roughness. Guard normalize against zero vectors.
    float3 n = float3(0.0, 0.0, 1.0);
    float roughness = 0.5;
    if (InputMask & 2u)
    {
        float4 nr = InNormalRoughness.Load(p);
        n = NormalsArePacked ? (nr.xyz * 2.0 - 1.0) : nr.xyz;
        n = (dot(n, n) > 1e-8) ? normalize(n) : float3(0.0, 0.0, 1.0);
        roughness = nr.w;
    }
    OutNormals[tid.xy] = float4(OctEncode(n), roughness, 0.0);

    // Albedo: linear pass-through; dispatch sets NON_GAMMA. Missing (bits2/3 clear) -> 0.
    float3 diff = (InputMask & 4u) ? InDiffuseAlbedo.Load(p).rgb : float3(0.0, 0.0, 0.0);
    float3 spec = (InputMask & 8u) ? InSpecularAlbedo.Load(p).rgb : float3(0.0, 0.0, 0.0);
    OutDiffuseAlbedo[tid.xy]  = float4(diff, 1.0);
    OutSpecularAlbedo[tid.xy] = float4(spec, 1.0);
    // Fused albedo is the MLD denoiser's internal (de)modulation guide. It must NEVER be 0: a zero guide
    // divides the radiance to black (the black-sky bug when a game feeds no albedo). Epsilon-clamp the real
    // albedo (the oracle adds +0.01); use a neutral 1.0 when no albedo was provided -> identity guide.
    float3 fused = ((InputMask & (4u | 8u)) != 0u) ? max(max(spec, diff), 0.01) : float3(1.0, 1.0, 1.0);
    OutFusedAlbedo[tid.xy] = float4(fused, 1.0);

    // Radiance (noisy). Optional per-game demodulation by fused albedo (tune in-game).
    float3 color = InColor.Load(p).rgb;
    if (DemodulateRadiance != 0)
        color = color / max(fused, 1e-4);
    OutRadiance[tid.xy] = float4(color, 0.0);
}
)";
