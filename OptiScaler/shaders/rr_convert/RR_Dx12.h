#pragma once

#include "SysUtils.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>
#include "RR_Common.h"

#define RR_NUM_OF_HEAPS 2
#define RR_NUM_OUTPUTS 8

// NGX Ray-Reconstruction -> FFX-MLD 1-signal input conversion (6 SRV inputs -> 8 UAV outputs).
// The 7 outputs are the MLD dispatch inputs; the denoised result is written by MLD to the app's
// output target, not here. See RayRegenFeature_Dx12 + OPTISCALER_RR_PLAN.md "Path B".
class RR_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[RR_NUM_OF_HEAPS];

    // u0..u6 in shader order.
    ID3D12Resource* _outputs[RR_NUM_OUTPUTS] = {};
    D3D12_RESOURCE_STATES _outputStates[RR_NUM_OUTPUTS] = {};

    uint32_t _width = 0;
    uint32_t _height = 0;

    uint32_t InNumThreadsX = 8;
    uint32_t InNumThreadsY = 8;

  public:
    // Allocate the 7 output textures sized to InWidth x InHeight (heap props copied from InRef).
    bool CreateBufferResources(ID3D12Device* InDevice, ID3D12Resource* InRef, uint32_t InWidth, uint32_t InHeight);

    // Barrier all outputs to InState (e.g. UNORDERED_ACCESS before dispatch, COMPUTE_READ before MLD).
    void TransitionOutputs(ID3D12GraphicsCommandList* InCmdList, D3D12_RESOURCE_STATES InState);

    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, const RRConstants& InConstants, ID3D12Resource* InColor,
                  ID3D12Resource* InDepth, ID3D12Resource* InMotionVectors, ID3D12Resource* InNormalRoughness,
                  ID3D12Resource* InDiffuseAlbedo, ID3D12Resource* InSpecularAlbedo);

    ID3D12Resource* Radiance() { return _outputs[0]; }
    ID3D12Resource* FusedAlbedo() { return _outputs[1]; }
    ID3D12Resource* LinearDepth() { return _outputs[2]; }
    ID3D12Resource* MotionVectors() { return _outputs[3]; }
    ID3D12Resource* Normals() { return _outputs[4]; }
    ID3D12Resource* SpecularAlbedo() { return _outputs[5]; }
    ID3D12Resource* DiffuseAlbedo() { return _outputs[6]; }
    ID3D12Resource* SkipSignal() { return _outputs[7]; }

    bool CanRender() const { return _init && _outputs[0] != nullptr; }

    RR_Dx12(std::string InName, ID3D12Device* InDevice);
    ~RR_Dx12();
};
