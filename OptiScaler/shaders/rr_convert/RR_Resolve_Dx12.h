#pragma once

#include "SysUtils.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>
#include "RR_Resolve_Common.h"

#define RR_RESOLVE_NUM_OF_HEAPS 2

// Post-denoise resolve pass: recomposition (sky skip-signal) + debug visualization.
//
// Owns the intermediate "denoised" buffer that the MLD denoiser writes into (instead of the app
// output target), so this pass can composite the sky back over it before presenting. The 6 other
// SRV inputs come from the conversion pass (RR_Dx12); the UAV output is the app's output target.
// See RR_Resolve_Common.h and RayRegenFeature_Dx12.
class RR_Resolve_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[RR_RESOLVE_NUM_OF_HEAPS];

    // The MLD denoiser writes here (UAV); this pass reads it (SRV t0) and writes the app output.
    ID3D12Resource* _denoised = nullptr;
    D3D12_RESOURCE_STATES _denoisedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    uint32_t _width = 0;
    uint32_t _height = 0;

    uint32_t InNumThreadsX = 8;
    uint32_t InNumThreadsY = 8;

  public:
    // Allocate the denoised intermediate sized to InWidth x InHeight (heap props copied from InRef).
    bool CreateBufferResources(ID3D12Device* InDevice, ID3D12Resource* InRef, uint32_t InWidth, uint32_t InHeight);

    // The MLD denoiser output target. Put it in UNORDERED_ACCESS first (PrepareForDenoiser).
    ID3D12Resource* Denoised() { return _denoised; }
    void PrepareForDenoiser(ID3D12GraphicsCommandList* InCmdList);

    // Recompose (or debug-visualize) into InOutput. The conversion outputs must already be in a
    // shader-read state (RR_Dx12::TransitionOutputs). _denoised is transitioned to read here.
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, const RRResolveConstants& InConstants,
                  ID3D12Resource* InSkipSignal, ID3D12Resource* InRadiance, ID3D12Resource* InLinearDepth,
                  ID3D12Resource* InMotionVectors, ID3D12Resource* InNormals, ID3D12Resource* InFusedAlbedo,
                  ID3D12Resource* InOutput);

    bool CanRender() const { return _init && _denoised != nullptr; }

    RR_Resolve_Dx12(std::string InName, ID3D12Device* InDevice);
    ~RR_Resolve_Dx12();
};
