#pragma once

#include "SysUtils.h"

// Constants for the NGX Ray-Reconstruction -> FFX-MLD input conversion shader.
// Field order/layout must match the cbuffer in shaderCode below.
struct alignas(256) RRConstants
{
    uint32_t RenderWidth;
    uint32_t RenderHeight;
    float MotionScaleX;       // multiply InMotionVectors.x to get UV-space (PreviousUV - CurrentUV)
    float MotionScaleY;       // multiply InMotionVectors.y
    float DepthLinA;          // linearDepth = 1 / (DepthLinA * deviceDepth + DepthLinB)
    float DepthLinB;
    uint32_t NormalsArePacked; // 1 if normals are stored as n * 0.5 + 0.5
    uint32_t DemodulateRadiance; // 1 to divide the noisy colour by fused albedo (per-game; tune in-game)
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
    float DepthLinA;
    float DepthLinB;
    uint  NormalsArePacked;
    uint  DemodulateRadiance;
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

    // Device depth -> absolute linear depth.
    float dd = InDepth.Load(p);
    float linDepth = 1.0 / (DepthLinA * dd + DepthLinB);
    OutLinearDepth[tid.xy] = abs(linDepth);

    // Motion vectors -> UV space (PreviousUV - CurrentUV). B = depth delta (needs history; 0 for now).
    float2 mv = InMotionVectors.Load(p).xy;
    OutMotionVectors[tid.xy] = float4(mv.x * MotionScaleX, mv.y * MotionScaleY, 0.0, 0.0);

    // Normal (+ roughness) -> octahedral RG, B roughness, A material type (0 = default).
    float4 nr = InNormalRoughness.Load(p);
    float3 n = NormalsArePacked ? (nr.xyz * 2.0 - 1.0) : nr.xyz;
    n = normalize(n);
    OutNormals[tid.xy] = float4(OctEncode(n), nr.w, 0.0);

    // Albedo: pass through linear; dispatch sets NON_GAMMA so no sqrt encoding is applied here.
    float3 diff = InDiffuseAlbedo.Load(p).rgb;
    float3 spec = InSpecularAlbedo.Load(p).rgb;
    OutDiffuseAlbedo[tid.xy]  = float4(diff, 1.0);
    OutSpecularAlbedo[tid.xy] = float4(spec, 1.0);
    float3 fused = max(spec, diff);
    OutFusedAlbedo[tid.xy] = float4(fused, 1.0);

    // Radiance (noisy). Optional per-game demodulation by fused albedo (tune in-game).
    float3 color = InColor.Load(p).rgb;
    if (DemodulateRadiance != 0)
        color = color / max(fused, 1e-4);
    OutRadiance[tid.xy] = float4(color, 0.0);
}
)";
