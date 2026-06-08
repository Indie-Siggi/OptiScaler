#include "pch.h"
#include "RR_Dx12.h"

#include "RR_Common.h"

#include <Config.h>
#include <State.h>

namespace
{
// Output formats in shader (u0..u6) order. RGBA16F for HDR/normal/MV/albedo, R32F for linear depth.
constexpr DXGI_FORMAT kOutputFormats[RR_NUM_OUTPUTS] = {
    DXGI_FORMAT_R16G16B16A16_FLOAT, // u0 radiance
    DXGI_FORMAT_R16G16B16A16_FLOAT, // u1 fusedAlbedo
    DXGI_FORMAT_R32_FLOAT,          // u2 linearDepth
    DXGI_FORMAT_R16G16B16A16_FLOAT, // u3 motionVectors
    DXGI_FORMAT_R16G16B16A16_FLOAT, // u4 normals
    DXGI_FORMAT_R16G16B16A16_FLOAT, // u5 specularAlbedo
    DXGI_FORMAT_R16G16B16A16_FLOAT, // u6 diffuseAlbedo
    DXGI_FORMAT_R16G16B16A16_FLOAT, // u7 skipSignal (RGB original colour, A sky mask)
};

constexpr const wchar_t* kOutputNames[RR_NUM_OUTPUTS] = {
    L"RR_Radiance",      L"RR_FusedAlbedo",    L"RR_LinearDepth", L"RR_MotionVectors",
    L"RR_Normals",       L"RR_SpecularAlbedo", L"RR_DiffuseAlbedo", L"RR_SkipSignal",
};
} // namespace

bool RR_Dx12::CreateBufferResources(ID3D12Device* InDevice, ID3D12Resource* InRef, uint32_t InWidth, uint32_t InHeight)
{
    if (InDevice == nullptr || InRef == nullptr)
        return false;

    const auto flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                       D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    for (int i = 0; i < RR_NUM_OUTPUTS; i++)
    {
        if (!Shader_Dx12::CreateBufferResource(InDevice, InRef, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &_outputs[i],
                                               flags, InWidth, InHeight, kOutputFormats[i]))
        {
            LOG_ERROR("[{0}] Failed to create output {1}", _name, i);
            return false;
        }

        _outputs[i]->SetName(kOutputNames[i]);
        _outputStates[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    _width = InWidth;
    _height = InHeight;
    return true;
}

void RR_Dx12::TransitionOutputs(ID3D12GraphicsCommandList* InCmdList, D3D12_RESOURCE_STATES InState)
{
    D3D12_RESOURCE_BARRIER barriers[RR_NUM_OUTPUTS] = {};
    UINT count = 0;

    for (int i = 0; i < RR_NUM_OUTPUTS; i++)
    {
        if (_outputs[i] == nullptr || _outputStates[i] == InState)
            continue;

        barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[count].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[count].Transition.pResource = _outputs[i];
        barriers[count].Transition.StateBefore = _outputStates[i];
        barriers[count].Transition.StateAfter = InState;
        barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        _outputStates[i] = InState;
        count++;
    }

    if (count > 0)
        InCmdList->ResourceBarrier(count, barriers);
}

bool RR_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, const RRConstants& InConstants, ID3D12Resource* InColor,
                       ID3D12Resource* InDepth, ID3D12Resource* InMotionVectors, ID3D12Resource* InNormalRoughness,
                       ID3D12Resource* InDiffuseAlbedo, ID3D12Resource* InSpecularAlbedo)
{
    if (!_init || _device == nullptr || InCmdList == nullptr || !CanRender())
        return false;

    if (InColor == nullptr || InDepth == nullptr || InMotionVectors == nullptr || InNormalRoughness == nullptr ||
        InDiffuseAlbedo == nullptr || InSpecularAlbedo == nullptr)
    {
        LOG_ERROR("[{0}] missing input resource", _name);
        return false;
    }

    // Outputs must be writable for the compute pass.
    TransitionOutputs(InCmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    _counter = (_counter + 1) % RR_NUM_OF_HEAPS;
    FrameDescriptorHeap& heap = _frameHeaps[_counter];

    // SRVs t0..t5
    CreateShaderResourceView(_device, InColor, heap.GetSrvCPU(0));
    CreateShaderResourceView(_device, InDepth, heap.GetSrvCPU(1));
    CreateShaderResourceView(_device, InMotionVectors, heap.GetSrvCPU(2));
    CreateShaderResourceView(_device, InNormalRoughness, heap.GetSrvCPU(3));
    CreateShaderResourceView(_device, InDiffuseAlbedo, heap.GetSrvCPU(4));
    CreateShaderResourceView(_device, InSpecularAlbedo, heap.GetSrvCPU(5));

    // UAVs u0..u6
    for (int i = 0; i < RR_NUM_OUTPUTS; i++)
        CreateUnorderedAccessView(_device, _outputs[i], heap.GetUavCPU(i), 0);

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

RR_Dx12::RR_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    if (!SetupRootSignature(InDevice, 6, RR_NUM_OUTPUTS, 1))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(RRConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    auto result =
        InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&_constantBuffer));

    if (result != S_OK)
    {
        LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
        return;
    }

    // Runtime-compiles shaderCode when UsePrecompiledShaders=false. TODO: ship a precompiled
    // RR_cso (precompiled/RR_Shader.h via shader_tools/dxc) for the precompiled path.
    if (!CreateComputePipeline(InDevice, &_pipelineState, nullptr, 0, shaderCode.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, RR_NUM_OF_HEAPS);
}

RR_Dx12::~RR_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < RR_NUM_OF_HEAPS; i++)
        _frameHeaps[i].ReleaseHeaps();

    for (int i = 0; i < RR_NUM_OUTPUTS; i++)
        SAFE_RELEASE(_outputs[i]);
}
