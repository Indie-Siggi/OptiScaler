#pragma once

#include "SysUtils.h"

// InputMask bits: which optional GBuffer inputs the game provided. Missing inputs are bound to a valid
// stand-in resource and ignored via the mask (neutral defaults). Keep in sync with shaderCode below.
#define RR_INPUT_HAS_MOTIONVECTORS   1u
#define RR_INPUT_HAS_NORMALS         2u
#define RR_INPUT_HAS_DIFFUSE_ALBEDO  4u
#define RR_INPUT_HAS_SPECULAR_ALBEDO 8u
#define RR_INPUT_ALL                 15u
#define RR_INPUT_HAS_SPEC_HITDIST    16u // optional: specular ray length -> OutRadiance.a

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
    float SkyThreshold;          // viewZ >= FarPlane * SkyThreshold marks a sky pixel for the skip-signal
    uint32_t DebugCapture;       // 1 = sample 2 pixels (centre + sky) into OutDebug for CPU readback/logging
    // Matrix-derived depth: viewZ = (B - dd*D) / (dd*C - A), from the ViewToClipMatrix z/w mapping
    // (A=M[2][2], B=M[3][2], C=M[2][3], D=M[3][3]). Convention-proof vs the guessed near/far closed form.
    float DepthMatA;
    float DepthMatB;
    float DepthMatC;
    float DepthMatD;
    uint32_t HasDepthMatrix;     // 1 = use the matrix coefficients; 0 = fall back to the near/far closed form
    uint32_t HasPrevDepth;       // 1 = compute the motion-vector .z depth delta from InPrevLinearDepth (history)
};

// OutDebug layout: 2 sample pixels x 4 float4 each (centre at base 0, sky at base 4):
//   [0] (pixelX, pixelY, rawDeviceDepth, viewZ)   [1] (normal.xyz, roughness)
//   [2] (outMV.xy, fusedAlbedo.r, skyMask)        [3] (radiance.rgb, clampedLinearDepth)
#define RR_DEBUG_FLOAT4_COUNT 8

// HLSL converter. Runtime-compiled as cs_5_0 when UsePrecompiledShaders=false; otherwise the
// precompiled RR_cso (precompiled/RR_Shader.h, generated via shader_tools/dxc) is used.
//
// Produces the MLD 1-signal inputs from the intercepted DLSS-RR buffers:
//   linearDepth (abs linear), motionVectors (RG=UV, B=depth delta), normals (RG=octahedral,
//   B=linear roughness, A=material), specular/diffuse albedo (sqrt-encoded; NON_GAMMA OFF),
//   fusedAlbedo = sqrt(max(spec,diff)), radiance (DEMODULATED colour = color/fusedLinear; the
//   resolve pass re-modulates). See OPTISCALER_RR_PLAN.md Appendix A3.6.
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
    float SkyThreshold;       // viewZ >= FarPlane * SkyThreshold marks a sky pixel
    uint  DebugCapture;       // 1 = write the centre + sky sample into OutDebug
    float DepthMatA;          // viewZ = (B - dd*D) / (dd*C - A) from the ViewToClip z/w mapping
    float DepthMatB;
    float DepthMatC;
    float DepthMatD;
    uint  HasDepthMatrix;     // 1 = use the matrix coefficients; 0 = near/far closed form
    uint  HasPrevDepth;       // 1 = compute MV .z depth delta from InPrevLinearDepth
};

Texture2D<float4> InColor           : register(t0);
Texture2D<float>  InDepth           : register(t1);
Texture2D<float4> InMotionVectors   : register(t2);
Texture2D<float4> InNormalRoughness : register(t3); // RGB normal, A linear roughness
Texture2D<float4> InDiffuseAlbedo   : register(t4);
Texture2D<float4> InSpecularAlbedo  : register(t5);
Texture2D<float4> InSpecHitDist     : register(t6); // R specular ray length (hit distance)
Texture2D<float>  InPrevLinearDepth : register(t7); // R previous-frame abs linear depth (for the MV depth delta)

RWTexture2D<float4> OutRadiance       : register(u0); // RGB noisy radiance, A specular ray length
RWTexture2D<float4> OutFusedAlbedo    : register(u1); // RGB max(spec,diff)
RWTexture2D<float>  OutLinearDepth    : register(u2); // R abs linear depth
RWTexture2D<float4> OutMotionVectors  : register(u3); // RG UV motion, B depth delta
RWTexture2D<float4> OutNormals        : register(u4); // RG octahedral, B roughness, A material
RWTexture2D<float4> OutSpecularAlbedo : register(u5);
RWTexture2D<float4> OutDiffuseAlbedo  : register(u6);
RWTexture2D<float4> OutSkipSignal     : register(u7); // RGB original colour, A sky mask (1 = sky); for recomposition
RWStructuredBuffer<float4> OutDebug   : register(u8); // CPU-readback sample buffer (see RR_DEBUG_FLOAT4_COUNT)

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

    // Device depth -> abs linear view-space depth, clamped to [near, far] (what FFX-MLD wants for linearDepth).
    // Preferred: invert the real ViewToClip z/w mapping, viewZ = (B - dd*D)/(dd*C - A) (handles reversed-Z and
    // infinite-far automatically; coefficients are the projection matrix Cyberpunk provides). Fallback: the old
    // near/far closed form if no matrix was available. See RR_BUILD_DEPLOY.md + FFX ffx_denoiser.h.
    float dd = InDepth.Load(p);
    float viewZ;
    if (HasDepthMatrix != 0u)
    {
        float denomM = dd * DepthMatC - DepthMatA;
        viewZ = (DepthMatB - dd * DepthMatD) / ((abs(denomM) > 1e-9) ? denomM : 1e-9);
    }
    else
    {
        float depthSpan = FarPlane - NearPlane;
        float denom = (ReversedZ != 0u) ? (NearPlane + dd * depthSpan) : (FarPlane - dd * depthSpan);
        viewZ = (NearPlane * FarPlane) / max(denom, 1e-6);
    }
    viewZ = abs(viewZ);
    float linDepth = clamp(viewZ, NearPlane, FarPlane);
    OutLinearDepth[tid.xy] = linDepth;

    float3 color = InColor.Load(p).rgb;

    // Sky / far-plane skip-signal for the post-denoise recomposition pass. The MLD denoiser zeroes
    // far-plane (sky) pixels even with finite depth + neutral albedo (the persistent black sky), so flag
    // sky pixels here and carry the original colour; the resolve pass lerps it back over the denoised
    // result. Reproduces the DarkHelmet oracle's skip-signal sky bypass. See RR_BUILD_DEPLOY.md.
    float skyMask = (viewZ >= FarPlane * SkyThreshold) ? 1.0 : 0.0;
    OutSkipSignal[tid.xy] = float4(color, skyMask);

    // Motion vectors -> UV space (PreviousUV - CurrentUV). B = abs linear depth delta (abs(prevLinZ) -
    // abs(curLinZ)): reproject to the previous pixel via the MV and sample last frame's linear depth.
    // Missing MV (bit0 clear) -> zero motion (denoiser treats the pixel as static).
    float2 mv = (InputMask & 1u) ? InMotionVectors.Load(p).xy : float2(0.0, 0.0);
    float2 mvUV = float2(mv.x * MotionScaleX, mv.y * MotionScaleY);
    float depthDelta = 0.0;
    if (HasPrevDepth != 0u)
    {
        float2 prevPx = float2(tid.xy) + mvUV * float2((float) RenderWidth, (float) RenderHeight);
        int2 ppi = clamp(int2(prevPx + 0.5), int2(0, 0), int2((int) RenderWidth - 1, (int) RenderHeight - 1));
        float prevLinZ = InPrevLinearDepth.Load(int3(ppi, 0));
        depthDelta = abs(prevLinZ) - abs(linDepth);
    }
    OutMotionVectors[tid.xy] = float4(mvUV, depthDelta, 0.0);

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

    // Albedo guides -> sqrt-encoded to match AMD's MLD convention (trace_rays_denoiser.hlsl:265-266,280;
    // dispatch leaves NON_GAMMA OFF). Keep the LINEAR albedo (`fused`) for the radiance demod/remod math.
    // Missing (bits2/3 clear) -> 0.
    float3 diff = (InputMask & 4u) ? InDiffuseAlbedo.Load(p).rgb : float3(0.0, 0.0, 0.0);
    float3 spec = (InputMask & 8u) ? InSpecularAlbedo.Load(p).rgb : float3(0.0, 0.0, 0.0);
    OutDiffuseAlbedo[tid.xy]  = float4(sqrt(max(diff, 0.0)), 1.0);
    OutSpecularAlbedo[tid.xy] = float4(sqrt(max(spec, 0.0)), 1.0);
    // `fused` is the LINEAR fused albedo: it demodulates the radiance here and re-modulates it in the resolve.
    // It must NEVER be 0: a zero guide divides the radiance to black (the black-sky bug when a game feeds no
    // albedo). Epsilon-clamp the real albedo (the oracle adds +0.01); neutral 1.0 when none -> identity guide.
    // The stored guide is sqrt(fused) (AMD stores sqrt(fusedAlbedo) and decodes via Square in compose).
    float3 fused = ((InputMask & (4u | 8u)) != 0u) ? max(max(spec, diff), 0.01) : float3(1.0, 1.0, 1.0);
    OutFusedAlbedo[tid.xy] = float4(sqrt(fused), 1.0);

    // Radiance (noisy). FFX 1-signal denoises in DEMODULATED (lighting) space: feed color / fusedLinear and
    // let the resolve pass re-modulate the denoised result by the same fused (AMD: trace_rays_denoiser.hlsl:274
    // + denoiser_compose.hlsl:156). RGB demodulated radiance + A = specular ray length (in-only), so feed the
    // specular hit distance into .a. DemodulateRadiance MUST stay paired with the resolve's ReModulate flag.
    float3 radiance = color;
    if (DemodulateRadiance != 0)
        radiance = radiance / max(fused, 1e-4);
    float specHitDist = (InputMask & 16u) ? InSpecHitDist.Load(p).r : 0.0;
    OutRadiance[tid.xy] = float4(radiance, specHitDist);

    // Debug capture: the threads at the centre and a near-top (sky) pixel write their input + output
    // values into OutDebug for the CPU to read back and log. Lets us see the actual converted numbers
    // (raw vs linear depth, normal, motion, fused albedo, sky mask) without RenderDoc.
    if (DebugCapture != 0u)
    {
        uint base = 0xffffffffu;
        if (tid.x == RenderWidth / 2u && tid.y == RenderHeight / 2u)
            base = 0u; // screen centre
        else if (tid.x == RenderWidth / 2u && tid.y == RenderHeight / 20u)
            base = 4u; // near top -> usually sky
        if (base != 0xffffffffu)
        {
            OutDebug[base + 0u] = float4((float) tid.x, (float) tid.y, dd, viewZ);
            OutDebug[base + 1u] = float4(n, roughness);
            OutDebug[base + 2u] = float4(mv.x * MotionScaleX, mv.y * MotionScaleY, fused.r, skyMask);
            OutDebug[base + 3u] = float4(radiance, clamp(viewZ, NearPlane, FarPlane));
        }
    }
}
)";
