#include "pch.h"
#include "RR_Resolve_Dx12.h"

#include "RR_Resolve_Common.h"

#include <Config.h>
#include <State.h>

namespace
{
// HDR intermediate the denoiser writes into; resolve reads it and writes the app output.
constexpr DXGI_FORMAT kDenoisedFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
} // namespace

bool RR_Resolve_Dx12::CreateBufferResources(ID3D12Device* InDevice, ID3D12Resource* InRef, uint32_t InWidth,
                                            uint32_t InHeight)
{
    if (InDevice == nullptr || InRef == nullptr)
        return false;

    const auto flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                       D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    if (!Shader_Dx12::CreateBufferResource(InDevice, InRef, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &_denoised, flags,
                                           InWidth, InHeight, kDenoisedFormat))
    {
        LOG_ERROR("[{0}] Failed to create the denoised intermediate", _name);
        return false;
    }

    _denoised->SetName(L"RR_Denoised");
    _denoisedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    _width = InWidth;
    _height = InHeight;
    return true;
}

void RR_Resolve_Dx12::PrepareForDenoiser(ID3D12GraphicsCommandList* InCmdList)
{
    if (_denoised == nullptr || InCmdList == nullptr || _denoisedState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        return;

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(_denoised, _denoisedState,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    InCmdList->ResourceBarrier(1, &barrier);
    _denoisedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

bool RR_Resolve_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, const RRResolveConstants& InConstants,
                               ID3D12Resource* InSkipSignal, ID3D12Resource* InRadiance, ID3D12Resource* InLinearDepth,
                               ID3D12Resource* InMotionVectors, ID3D12Resource* InNormals, ID3D12Resource* InFusedAlbedo,
                               ID3D12Resource* InOutput)
{
    if (!_init || _device == nullptr || InCmdList == nullptr || !CanRender())
        return false;

    if (InSkipSignal == nullptr || InRadiance == nullptr || InLinearDepth == nullptr || InMotionVectors == nullptr ||
        InNormals == nullptr || InFusedAlbedo == nullptr || InOutput == nullptr)
    {
        LOG_ERROR("[{0}] missing input resource", _name);
        return false;
    }

    // The denoiser wrote _denoised as a UAV; make it readable for this pass.
    if (_denoisedState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(_denoised, _denoisedState,
                                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        InCmdList->ResourceBarrier(1, &barrier);
        _denoisedState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    _counter = (_counter + 1) % RR_RESOLVE_NUM_OF_HEAPS;
    FrameDescriptorHeap& heap = _frameHeaps[_counter];

    // SRVs t0..t6
    CreateShaderResourceView(_device, _denoised, heap.GetSrvCPU(0));
    CreateShaderResourceView(_device, InSkipSignal, heap.GetSrvCPU(1));
    CreateShaderResourceView(_device, InRadiance, heap.GetSrvCPU(2));
    CreateShaderResourceView(_device, InLinearDepth, heap.GetSrvCPU(3));
    CreateShaderResourceView(_device, InMotionVectors, heap.GetSrvCPU(4));
    CreateShaderResourceView(_device, InNormals, heap.GetSrvCPU(5));
    CreateShaderResourceView(_device, InFusedAlbedo, heap.GetSrvCPU(6));

    // UAV u0 (app output target)
    CreateUnorderedAccessView(_device, InOutput, heap.GetUavCPU(0), 0);

    // CBV b0
    if (!CreateConstantsBuffer(_device, _constantBuffer, InConstants, heap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());

    const UINT dispatchWidth = (_width + InNumThreadsX - 1) / InNumThreadsX;
    const UINT dispatchHeight = (_height + InNumThreadsY - 1) / InNumThreadsY;

    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

RR_Resolve_Dx12::RR_Resolve_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    // 7 SRVs (denoised + 6 conversion outputs), 1 UAV (app output), 1 CBV.
    if (!SetupRootSignature(InDevice, 7, 1, 1))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(RRResolveConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    auto result =
        InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&_constantBuffer));

    if (result != S_OK)
    {
        LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
        return;
    }

    if (!CreateComputePipeline(InDevice, &_pipelineState, nullptr, 0, resolveShaderCode.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, RR_RESOLVE_NUM_OF_HEAPS);
}

RR_Resolve_Dx12::~RR_Resolve_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < RR_RESOLVE_NUM_OF_HEAPS; i++)
        _frameHeaps[i].ReleaseHeaps();

    SAFE_RELEASE(_denoised);
}
