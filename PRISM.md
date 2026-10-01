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
  and keeps a blended haze where the leaves were. The settings below (History Clamp, Anti-Ghost, Disocclusion Gate)
  do not fix it; the Prism README explains the analysis.

## Building

1. Build libprism as a static library on Windows: in a Prism checkout, copy the shaders built on Linux
   (`build/shaders/*.spv.h`) into `prebuilt-shaders/` and run `interop\build-windows.bat <this OptiScaler checkout>`.
2. Build OptiScaler as usual, adding `/p:PrismDir=<Prism checkout>`:
   `msbuild OptiScaler.sln /p:Configuration=Release /p:Platform=x64 /p:PrismDir=C:\path\to\prism-nr`.
   Without `PrismDir` the backend is compiled out and reports that it is unavailable.

## Using it

- NSS's weights are not included (they are under the Arm AI Model Community License). Download them with Prism's
  `tools/fetch_nss.sh`, export the network as a Prism model (see the Prism README) and set `[Prism] ModelPath` to
  that folder. The path has to be visible to the game: a Flatpak launcher may not see your home directory.
- Select it with `[Upscalers] Dx12Upscaler=prism` or in the overlay.
- `[Prism]` in `OptiScaler.ini` documents every setting: exposure, jitter and motion-vector scales, the history
  clamp, anti-ghost and disocclusion-gate experiments, and the frame capture. The overlay has the same settings
  plus debug views (theta, alpha, gamma, motion, gate).
- **Frame capture:** "Capture Frames" in the overlay writes the inputs NSS receives (color, motion, depth,
  parameters, the game's reactive mask) for the next frames, in the format `prism-cli nss-sequence` replays. About
  12 MB per frame at 960x540 render size, 50 MB at 1920x1080.

Input conventions found for Cyberpunk 2077 (the defaults handle them): motion vectors are UV-space with
`MV_Scale` = render size and get negated; the jitter is negated on both axes; depth is reversed-Z.

## License and credits

Same license as OptiScaler (GPL-3.0). Prism itself is Apache-2.0; NSS's network and processing are built from
Arm's Apache-2.0 model code, its weights are not. Developed with help from Anthropic's Claude as an AI
pair-programmer, under human direction and review.
