# FSR Ray Regeneration for DLSS Ray Reconstruction (branch `rr-mld-backend`)

This branch lets games that support NVIDIA DLSS Ray Reconstruction use **AMD FSR Ray Regeneration** instead, on
RDNA4 GPUs (RX 9000 series), on Windows and on Linux through Proton. It is an experimental research branch: it runs
in one game and crashes in another, as described below.

## How it works

When a game creates DLSS Ray Reconstruction (NGX DLSS-D) and the GPU supports AMD's denoiser, OptiScaler creates a
`RayRegenFeatureDx12` instead. Per frame:

1. A compute shader converts the game's DLSS-RR inputs into what AMD's denoiser (FidelityFX "MLD", the denoiser behind
   FSR Ray Regeneration, version 1.1.0) expects: linear depth, octahedral normals with roughness, sqrt-encoded
   albedo, UV motion vectors, and the noisy color divided by the albedo, since the denoiser works on lighting.
2. The denoiser runs in its single-signal mode.
3. A resolve pass multiplies the albedo back in and restores the sky, which bypasses the denoiser.
4. FSR anti-aliases the result, as in AMD's own pipeline: DLSS-RR also acts as the game's anti-aliasing, so the
   game hands it a jittered, un-anti-aliased frame.

## Status

- **Crimson Desert** (DLSS 4.5, Quality, Ray Reconstruction on): runs at the cost of the game's native FSR Ray
  Regeneration (the denoiser itself about 9 ms at 1440p render size on an RX 9070 XT). Open: ghosting on particles
  (flying leaves have no G-buffer data) and shimmer on character edges in motion.
- **Cyberpunk 2077:** crashes on the first Ray Reconstruction frame when the FSR pass is on
  (`[RayRegen] FsrAntiAliasing=false` runs, but without anti-aliasing the image is noisy).
- Only denoiser **1.1.0** (FidelityFX SDK 2.2) works: SDK 2.3 (Ray Regeneration 1.2.0) removed the single-signal API.
- Denoiser settings apply when the effect is created; changing them live is not supported.

## Setup

- Put AMD's FidelityFX SDK 2.2 DLLs in OptiScaler's dll folder or point `[Libraries]` at them: the loader
  (`FfxDx12Path`), the upscaler (`FfxDx12SRPath`) and denoiser 1.1.0 (`FfxDx12RRPath`).
- Enable Ray Reconstruction in the game. `[RayRegen]` in `OptiScaler.ini` documents every setting (input
  conventions, denoiser tuning, the FSR pass, debug views); the overlay has a live debug window.
- Settings that worked for Crimson Desert: `[Spoofing] Dxgi=true`, `[Inputs] EnableFfxInputs=false`,
  `[RayRegen] UseExposureTexture=true`, `Reprojection=true`, `NormalsArePacked=false`.

## License and credits

Same license as OptiScaler (GPL-3.0). `ffx_denoiser.h` and `ffx_denoiser.hpp` are vendored from AMD's FidelityFX
SDK 2.2 (MIT). The sky bypass follows DarkHelmet's OptiScaler fork. Developed with help from Anthropic's Claude as
an AI pair-programmer, under human direction and review.
