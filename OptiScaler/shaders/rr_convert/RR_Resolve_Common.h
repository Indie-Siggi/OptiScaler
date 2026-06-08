#pragma once

#include "SysUtils.h"

// Post-denoise resolve pass for the FSR Ray Regeneration backend.
//
// Two jobs, selected by DebugView:
//   DebugView == 0  -> RECOMPOSITION: composite the sky back over the denoised image using the
//                      conversion pass's skip-signal (the MLD denoiser zeroes far-plane/sky pixels;
//                      this restores them). final = lerp(denoised, skip.rgb, skyMask).
//   DebugView  > 0  -> DEBUG VISUALIZATION: write a chosen converted signal straight to the output
//                      (the denoiser is skipped that frame), to ground-truth the conversion in-game.
//
// Field order/layout must match the cbuffer in shaderCode below.
struct alignas(256) RRResolveConstants
{
    uint32_t RenderWidth;
    uint32_t RenderHeight;
    uint32_t DebugView; // 0 = recompose; 1..8 = visualize a converted signal (see shaderCode)
    float NearPlane;    // for normalizing the linear-depth debug view
    float FarPlane;
};

inline static std::string resolveShaderCode = R"(
cbuffer Params : register(b0)
{
    uint  RenderWidth;
    uint  RenderHeight;
    uint  DebugView;
    float NearPlane;
    float FarPlane;
};

Texture2D<float4> InDenoised : register(t0); // MLD denoiser output (intermediate)
Texture2D<float4> InSkip     : register(t1); // RGB original colour, A sky mask
Texture2D<float4> InRadiance : register(t2); // noisy radiance (conversion out)
Texture2D<float>  InDepth    : register(t3); // linear depth
Texture2D<float4> InMotion   : register(t4); // UV motion, B depth delta
Texture2D<float4> InNormals  : register(t5); // RG octahedral, B roughness
Texture2D<float4> InFused    : register(t6); // fused albedo guide

RWTexture2D<float4> OutColor : register(u0); // app output target

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= RenderWidth || tid.y >= RenderHeight)
        return;

    int3 p = int3(tid.xy, 0);

    if (DebugView == 0)
    {
        // Recomposition: pull the sky back in from the skip-signal where the denoiser zeroed it.
        float4 d = InDenoised.Load(p);
        float4 s = InSkip.Load(p);
        OutColor[tid.xy] = float4(lerp(d.rgb, s.rgb, saturate(s.a)), d.a);
        return;
    }

    float3 v = float3(0.0, 0.0, 0.0);
    if (DebugView == 1) // linear depth, normalized to [near, far]
    {
        float z = InDepth.Load(p);
        float n = saturate((z - NearPlane) / max(FarPlane - NearPlane, 1e-3));
        v = float3(n, n, n);
    }
    else if (DebugView == 2) // normals (octahedral .xy) -> colour
    {
        float2 oct = InNormals.Load(p).xy;
        v = float3(oct * 0.5 + 0.5, 0.0);
    }
    else if (DebugView == 3) // roughness (normals .z)
    {
        v = InNormals.Load(p).zzz;
    }
    else if (DebugView == 4) // motion vectors (amplified for visibility)
    {
        float2 mv = InMotion.Load(p).xy;
        v = float3(abs(mv) * 50.0, 0.0);
    }
    else if (DebugView == 5) // fused albedo guide
    {
        v = InFused.Load(p).rgb;
    }
    else if (DebugView == 6) // noisy radiance (conversion input to the denoiser)
    {
        v = InRadiance.Load(p).rgb;
    }
    else if (DebugView == 7) // sky mask (skip-signal alpha)
    {
        v = InSkip.Load(p).aaa;
    }
    else // 8+ : denoised passthrough
    {
        v = InDenoised.Load(p).rgb;
    }

    OutColor[tid.xy] = float4(v, 1.0);
}
)";
