# Prism NSS upscaler (branch `prism-nss-backend`)

This branch adds an experimental OptiScaler upscaler backend, **Prism**, that runs Arm's Neural Super Sampling
(NSS v1) through the [Prism](https://github.com/Indie-Siggi/prism-nr) runtime in place of a game's upscaler. It is a
research branch, not a proposal for upstream: it has been tested in one game on one GPU.

## How it works

Games running through Proton use vkd3d-proton, which translates Direct3D 12 to Vulkan. Prism is a Vulkan compute
runtime, so the backend takes the game's color, depth, motion vectors and output through vkd3d-proton's interop
interface (`ID3D12DXVKInteropDevice1`) and records NSS straight into the game's D3D12 command list
(`BeginVkCommandBufferInterop`). It only works under vkd3d-proton, i.e. on Linux with Proton.

## Status

- Runs in **Cyberpunk 2077** (DLSS inputs, Performance mode) on an RX 9070 XT: 5.4 ms per frame for 1080p to 4K.
- Known problem: **ghosting on thin wind-blown foliage** (palm fronds over the sky). The game draws those leaves as
  a screen-door dither and relies on its own temporal anti-aliasing to blend it; NSS was not trained on such content
  and keeps a blended haze where the leaves were. The post-processing settings (History Clamp, Anti-Ghost, Disocclusion Gate)
  do not fix it; the Prism README explains the analysis.

## Building and setup

The step-by-step guide is in the Prism repository:
[docs/optiscaler.md](https://github.com/Indie-Siggi/prism-nr/blob/main/docs/optiscaler.md). In short: build
libprism on Windows (`interop\build-windows.bat <this OptiScaler checkout>` in a Prism checkout), build OptiScaler
with `/p:PrismDir=<Prism checkout>`, export the NSS model with weights you download yourself (Arm AI Model Community
License), install the package into the game as `dxgi.dll`, and set `[Upscalers] Dx12Upscaler=prism` and
`[Prism] ModelPath`. Without `PrismDir` the backend is compiled out and reports that it is unavailable.

`[Prism]` in `OptiScaler.ini` documents every setting; the overlay has the same settings plus debug views and
"Capture Frames", which records the inputs NSS receives for offline replay with `prism-cli nss-sequence`.

Input conventions found for Cyberpunk 2077 (the defaults handle them): motion vectors are UV-space with
`MV_Scale` = render size and get negated; the jitter is negated on both axes; depth is reversed-Z.

## License and credits

Same license as OptiScaler (GPL-3.0). Prism itself is Apache-2.0; NSS's network and processing are built from
Arm's Apache-2.0 model code, its weights are not. Developed with help from Anthropic's Claude as an AI
pair-programmer, under human direction and review.
