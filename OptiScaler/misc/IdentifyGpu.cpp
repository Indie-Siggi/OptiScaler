#include "pch.h"
#include "IdentifyGpu.h"

#include <proxies/Dxgi_Proxy.h>
#include <proxies/D3d12_Proxy.h>
#include "nvapi/NvApiTypes.h"
#include <magic_enum.hpp>

#include <hooks/Amdxc64_Hooks.h>
using Microsoft::WRL::ComPtr;

// Prioritize Nvidia cards that can run DLSS and are connected to a display
void sortGpus(std::vector<GpuInformation>& gpus)
{
    std::sort(gpus.begin(), gpus.end(),
              [](const GpuInformation& a, const GpuInformation& b)
              {
                  auto isPreferredNvidia = [](const GpuInformation& gpu)
                  {
                      bool isNvidia = (gpu.vendorId == VendorId::Nvidia);
                      return isNvidia && gpu.dlssCapable && !gpu.noDisplayConnected;
                  };

                  bool aIsPreferred = isPreferredNvidia(a);
                  bool bIsPreferred = isPreferredNvidia(b);

                  // If one is a preferred and the other isn't then the preferred one should be sorted first
                  if (aIsPreferred != bIsPreferred)
                  {
                      return aIsPreferred;
                  }

                  if (a.softwareAdapter)
                      return false;

                  // Fallback on VRAM amount
                  return a.dedicatedVramInBytes > b.dedicatedVramInBytes;
              });
}
std::vector<GpuInformation> IdentifyGpu::checkGpuInfo()
{
    auto localCachedInfo = std::vector<GpuInformation> {};

    ScopedSkipSpoofing skipSpoofing {};

    DxgiProxy::Init();

    ComPtr<IDXGIFactory6> factory = nullptr;
    HRESULT result = S_FALSE;

    result = DxgiProxy::CreateDxgiFactory_()(__uuidof(factory), (IDXGIFactory**) factory.GetAddressOf());

    if (result != S_OK || factory == nullptr)
    {
        // Will land here if getPrimaryGpu/getAllGpus are called from within DLL_PROCESS_ATTACH
        LOG_ERROR("Failed to create DXGI Factory, GPU info will be inaccurate!");
        return localCachedInfo;
    }

    UINT adapterIndex = 0;
    DXGI_ADAPTER_DESC1 adapterDesc {};
    ComPtr<IDXGIAdapter1> adapter;

    DXGI_GPU_PREFERENCE gpuPreference = DXGI_GPU_PREFERENCE_UNSPECIFIED;
    if (Config::Instance()->PreferDedicatedGpu.value_or_default())
        gpuPreference = DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;

    while (factory->EnumAdapterByGpuPreference(adapterIndex, gpuPreference, IID_PPV_ARGS(&adapter)) == S_OK)
    {
        if (adapter == nullptr)
        {
            adapterIndex++;
            continue;
        }

        result = adapter->GetDesc1(&adapterDesc);

        if (result == S_OK)
        {
            GpuInformation gpuInfo;
            gpuInfo.luid = adapterDesc.AdapterLuid;
            gpuInfo.vendorId = (VendorId::Value) adapterDesc.VendorId;
            gpuInfo.deviceId = adapterDesc.DeviceId;
            gpuInfo.subsystemId = adapterDesc.SubSysId;
            gpuInfo.revisionId = adapterDesc.Revision;
            gpuInfo.dedicatedVramInBytes = adapterDesc.DedicatedVideoMemory;

            std::wstring szName(adapterDesc.Description);
            gpuInfo.name = wstring_to_string(szName);

            ComPtr<IDXGIVkInteropAdapter> interopAdapter;
            if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIVkInteropAdapter), (void**) &interopAdapter)))
            {
                gpuInfo.usesDxvk = true;

                // Try to get the real GPU info when using dxvk.conf to spoof
                ComPtr<IDXGIVkInteropFactory> interopFactory;
                if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIVkInteropFactory), (void**) &interopFactory)))
                {
                    VkInstance vkInst = VK_NULL_HANDLE;
                    VkPhysicalDevice vkPhysDev = VK_NULL_HANDLE;
                    interopAdapter->GetVulkanHandles(&vkInst, &vkPhysDev);

                    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = VK_NULL_HANDLE;
                    interopFactory->GetVulkanInstance(&vkInst, &vkGetInstanceProcAddr);

                    if (vkPhysDev)
                    {
                        PFN_vkGetPhysicalDeviceProperties pvkGetProps = VK_NULL_HANDLE;
                        if (vkGetInstanceProcAddr && vkInst)
                        {
                            pvkGetProps = (PFN_vkGetPhysicalDeviceProperties) vkGetInstanceProcAddr(
                                vkInst, "vkGetPhysicalDeviceProperties");
                        }

                        if (!pvkGetProps)
                        {
                            HMODULE hVulkan = GetModuleHandleA("vulkan-1.dll");
                            pvkGetProps = hVulkan ? (PFN_vkGetPhysicalDeviceProperties) GetProcAddress(
                                                        hVulkan, "vkGetPhysicalDeviceProperties")
                                                  : nullptr;
                        }

                        if (pvkGetProps)
                        {
                            VkPhysicalDeviceProperties props {};
                            pvkGetProps(vkPhysDev, &props);
                            if (props.vendorID != gpuInfo.vendorId)
                            {
                                LOG_INFO("DXVK: unspoofing vendorId {:04X} -> {:04X} ({})", (uint32_t) gpuInfo.vendorId,
                                         props.vendorID, props.deviceName);
                                gpuInfo.vendorId = (VendorId::Value) props.vendorID;
                                gpuInfo.deviceId = props.deviceID;
                                gpuInfo.name = props.deviceName;
                            }
                        }
                    }
                }
            }

            if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                gpuInfo.softwareAdapter = true;

            localCachedInfo.push_back(std::move(gpuInfo));
        }
        else
        {
            LOG_DEBUG("Can't get description of adapter: {}", adapterIndex);
        }

        adapterIndex++;
    }

    // We might be getting the correct ordering by default.
    // Trying to sort by vendor might cause issues if someone
    // has some old Nvidia card in their system for example.
    // sortGpus(localCachedInfo);

    for (auto& gpuInfo : localCachedInfo)
    {
        if (gpuInfo.vendorId == VendorId::Nvidia)
        {
            queryNvapi(gpuInfo);
        }

        SAFE_RELEASE(gpuInfo.d3d12device);
    }

    return localCachedInfo;
}

void IdentifyGpu::queryNvapi(GpuInformation& gpuInfo)
{
    auto nvapiModule = NtdllProxy::LoadLibraryExW_Ldr(L"nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

    // No nvapi, should not be nvidia, possibly external spoofing
    if (!nvapiModule)
        return;

    auto o_NvAPI_QueryInterface =
        (PFN_NvApi_QueryInterface) KernelBaseProxy::GetProcAddress_()(nvapiModule, "nvapi_QueryInterface");

    if (!o_NvAPI_QueryInterface)
    {
        NtdllProxy::FreeLibrary_Ldr(nvapiModule);
        return;
    }

    // Check for fakenvapi in system32, assume it's not nvidia if found
    if (o_NvAPI_QueryInterface(0x21382138))
    {
        NtdllProxy::FreeLibrary_Ldr(nvapiModule);
        return;
    }

    auto* init = GET_INTERFACE(NvAPI_Initialize, o_NvAPI_QueryInterface);
    if (!init || init() != NVAPI_OK)
    {
        LOG_ERROR("Failed to init NvApi");
        NtdllProxy::FreeLibrary_Ldr(nvapiModule);
        return;
    }

    // Handle we want to grab
    NvPhysicalGpuHandle hPhysicalGpu {};

    // Grab logical GPUs to extract coresponding LUID
    auto* getLogicalGPUs = GET_INTERFACE(NvAPI_SYS_GetLogicalGPUs, o_NvAPI_QueryInterface);
    NV_LOGICAL_GPUS logicalGpus {};
    logicalGpus.version = NV_LOGICAL_GPUS_VER;
    if (getLogicalGPUs)
    {
        if (auto result = getLogicalGPUs(&logicalGpus); result != NVAPI_OK)
            LOG_ERROR("NvAPI_SYS_GetLogicalGPUs failed: {}", magic_enum::enum_name(result));
    }

    auto* getLogicalGpuInfo = GET_INTERFACE(NvAPI_GPU_GetLogicalGpuInfo, o_NvAPI_QueryInterface);

    if (getLogicalGpuInfo)
    {
        for (uint32_t i = 0; i < logicalGpus.gpuHandleCount; i++)
        {
            LUID luid;
            NV_LOGICAL_GPU_DATA logicalGpuData {};
            logicalGpuData.pOSAdapterId = &luid;
            logicalGpuData.version = NV_LOGICAL_GPU_DATA_VER;
            auto logicalGpu = logicalGpus.gpuHandleData[i].hLogicalGpu;

            if (auto result = getLogicalGpuInfo(logicalGpu, &logicalGpuData); result != NVAPI_OK)
                LOG_ERROR("NvAPI_GPU_GetLogicalGpuInfo failed: {}", magic_enum::enum_name(result));

            // We are looking at the correct GPU for this gpuInfo.luid
            if (IsEqualLUID(luid, gpuInfo.luid) && logicalGpuData.physicalGpuCount > 0)
            {
                if (logicalGpuData.physicalGpuCount > 1)
                    LOG_WARN("A logical GPU has more than a single physical GPU, we are only checking one");

                hPhysicalGpu = logicalGpuData.physicalGpuHandles[0];
            }
        }
    }

    auto* getArchInfo = GET_INTERFACE(NvAPI_GPU_GetArchInfo, o_NvAPI_QueryInterface);
    gpuInfo.nvidiaArchInfo.version = NV_GPU_ARCH_INFO_VER;
    if (getArchInfo && hPhysicalGpu && getArchInfo(hPhysicalGpu, &gpuInfo.nvidiaArchInfo) != NVAPI_OK)
        LOG_ERROR("Couldn't get GPU Architecture");

    auto* getConnectedDisplayIds = GET_INTERFACE(NvAPI_GPU_GetConnectedDisplayIds, o_NvAPI_QueryInterface);
    NvU32 displayCount = 0;
    if (getConnectedDisplayIds && hPhysicalGpu &&
        getConnectedDisplayIds(hPhysicalGpu, nullptr, &displayCount, 0) == NVAPI_OK && displayCount == 0)
    {
        gpuInfo.noDisplayConnected = true;
    }

    auto* unload = GET_INTERFACE(NvAPI_Unload, o_NvAPI_QueryInterface);
    if (!unload || unload() != NVAPI_OK)
        LOG_ERROR("Failed to unload NvApi");

    NtdllProxy::FreeLibrary_Ldr(nvapiModule);

    // assumes GTX16xx to be capable due to our spoofing
    if (Config::Instance()->DLSSEnabled.value_or_default())
        gpuInfo.dlssCapable = gpuInfo.nvidiaArchInfo.architecture_id >= NV_GPU_ARCHITECTURE_TU100;
}

void IdentifyGpu::getHardwareAdapter(IDXGIFactory* InFactory, IDXGIAdapter** InAdapter,
                                     D3D_FEATURE_LEVEL requiredFeatureLevel)
{
    LOG_FUNC();

    *InAdapter = nullptr;

    auto allGpus = getAllGpus();
    IDXGIFactory6* factory6 = nullptr;

    if (InFactory->QueryInterface(IID_PPV_ARGS(&factory6)) == S_OK && factory6 != nullptr)
    {
        D3d12Proxy::Init();

        for (auto gpu : allGpus)
        {
            if (*InAdapter == nullptr)
            {
                LOG_TRACE("Trying to select: {}", gpu.name);

                ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};
                auto result = factory6->EnumAdapterByLuid(gpu.luid, IID_PPV_ARGS(InAdapter));
            }

            if (*InAdapter != nullptr)
            {
                // Check if the requested D3D_FEATURE_LEVEL is supported without actually creating the device
                if (SUCCEEDED(D3d12Proxy::D3D12CreateDevice_()(*InAdapter, requiredFeatureLevel, _uuidof(ID3D12Device),
                                                               nullptr)))
                {
                    break;
                }

                (*InAdapter)->Release();
                *InAdapter = nullptr;
            }
        }

        factory6->Release();
    }
}

std::vector<GpuInformation> IdentifyGpu::getAllGpus()
{
    thread_local bool is_fetching = false;
    if (is_fetching)
        return {};

    std::scoped_lock lock(mutex);

    if (!cache.empty() && cache.front().deviceId != VendorId::Invalid)
    {
        return cache;
    }

    is_fetching = true;
    cache = checkGpuInfo();
    is_fetching = false;

    // TODO: try to reactivate DxgiSpoofing if was auto on Nvidia cards without DLSS
    return cache;
}

GpuInformation IdentifyGpu::getPrimaryGpu()
{
    auto allGpus = getAllGpus();
    return !allGpus.empty() ? allGpus.front() : GpuInformation {};
}

void IdentifyGpu::updateD3d12Capabilities(D3d12Proxy::PFN_D3D12CreateDevice o_D3D12CreateDevice)
{
    if (hasD3d12Capabilities)
        return;

    // Making sure the cache is filled
    getAllGpus();

    auto d3d12Module = KernelBaseProxy::GetModuleHandleW_()(L"d3d12.dll");
    if (!d3d12Module)
        return;

    D3d12Proxy::Init(d3d12Module);

    {
        std::scoped_lock lock(mutex);
        if (hasD3d12Capabilities)
            return;
        hasD3d12Capabilities = true;
    }

    struct D3d12Result
    {
        LUID luid;
        bool usesVkd3dProton = false;
        bool fsr4Capable = false;
    };
    std::vector<D3d12Result> results;

    for (auto& gpuInfo : cache)
    {
        if (gpuInfo.vendorId != VendorId::AMD && !gpuInfo.usesDxvk)
            continue;

        ComPtr<IDXGIFactory4> factory;
        if (FAILED(DxgiProxy::CreateDxgiFactory_()(__uuidof(factory), (IDXGIFactory**) factory.GetAddressOf())))
            continue;

        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(factory->EnumAdapterByLuid(gpuInfo.luid, IID_PPV_ARGS(&adapter))))
        {
            D3d12Proxy::PFN_D3D12CreateDevice pD3D12CreateDevice = nullptr;
            if (o_D3D12CreateDevice)
                pD3D12CreateDevice = o_D3D12CreateDevice;
            else
                pD3D12CreateDevice = D3d12Proxy::D3D12CreateDevice_();

            // D3D12 device is needed to be able to query amdxc and check for vkd3d-proton
            ScopedCreatingD3DDevice scopedCreating {};
            ScopedSkipVulkanHooks skipVulkanHooks {};
            ID3D12Device* localDevice = nullptr;
            auto createResult = pD3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&localDevice));

            if (SUCCEEDED(createResult) && localDevice)
            {
                D3d12Result res;
                res.luid = gpuInfo.luid;

                ComPtr<ID3D12DXVKInteropDevice> vkd3dInterop;
                if (localDevice && SUCCEEDED(localDevice->QueryInterface(IID_PPV_ARGS(&vkd3dInterop))))
                    res.usesVkd3dProton = true;

                if (gpuInfo.vendorId == VendorId::AMD)
                {
                    // Kinda questionable, may need to reconsider
                    if (Config::Instance()->Fsr4ForceCapable.value_or_default())
                        res.fsr4Capable = true;

                    // Query vkd3d-proton for extensions it's using to look for the required one for FSR 4
                    if (!res.fsr4Capable && res.usesVkd3dProton)
                    {
                        UINT extensionCount = 0;

                        if (SUCCEEDED(vkd3dInterop->GetDeviceExtensions(&extensionCount, nullptr)) &&
                            extensionCount > 0)
                        {
                            std::vector<const char*> exts(extensionCount);

                            if (SUCCEEDED(vkd3dInterop->GetDeviceExtensions(&extensionCount, exts.data())))
                            {
                                for (UINT i = 0; i < extensionCount; i++)
                                {
                                    // Only RDNA4+
                                    if (!strcmp("VK_EXT_shader_float8", exts[i]))
                                    {
                                        res.fsr4Capable = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    // Pre-RDNA4 GPUs on Linux can support FSR 4 but require a special envvar
                    // check for the envvar and assume everything else is also setup for FSR 4 to work on those
                    // cards
                    if (!res.fsr4Capable)
                    {
                        const char* envvar = getenv("DXIL_SPIRV_CONFIG");
                        if (envvar && strstr(envvar, "wmma_rdna3_workaround"))
                            res.fsr4Capable = true;
                    }

                    if (!res.fsr4Capable)
                    {
                        auto moduleAmdxc64 = KernelBaseProxy::GetModuleHandleW_()(L"amdxc64.dll");

                        if (moduleAmdxc64 == nullptr)
                            moduleAmdxc64 = NtdllProxy::LoadLibraryExW_Ldr(L"amdxc64.dll", NULL, 0);

                        if (moduleAmdxc64 == nullptr)
                            continue;

                        ComPtr<IAmdExtD3DFactory> amdExtD3DFactory = nullptr;
                        auto AmdExtD3DCreateInterface =
                            (PFN_AmdExtD3DCreateInterface) KernelBaseProxy::GetProcAddress_()(
                                moduleAmdxc64, "AmdExtD3DCreateInterface");

                        // Query amdxc for a specific intrinsics support, FSR 4 checks more but hopefully this one
                        // is enough amdxc on Windows hates vkd3d-proton's device, on Linux it's fine
                        if (!res.fsr4Capable && localDevice &&
                            (State::Instance().isRunningOnLinux || !res.usesVkd3dProton) && AmdExtD3DCreateInterface)
                        {
                            if (SUCCEEDED(AmdExtD3DCreateInterface(localDevice, IID_PPV_ARGS(&amdExtD3DFactory))))
                            {
                                ComPtr<IAmdExtD3DShaderIntrinsics> amdExtD3DShaderIntrinsics = nullptr;

                                if (amdExtD3DFactory && SUCCEEDED(amdExtD3DFactory->CreateInterface(
                                                            localDevice, IID_PPV_ARGS(&amdExtD3DShaderIntrinsics))))
                                {
                                    HRESULT float8support = amdExtD3DShaderIntrinsics->CheckSupport(
                                        AmdExtD3DShaderIntrinsicsSupport_Float8Conversion);
                                    res.fsr4Capable = float8support == S_OK;
                                }
                            }
                        }
                    }

                    // TODO: could now try to ask amdxcffx for FSR 4 and see if it returns it
                    // but our FSR 4 upgrade code call this function so it gets complicated
                }

                localDevice->Release();
                results.push_back(res);
            }
        }
    }

    {
        std::scoped_lock lock(mutex);
        for (auto& res : results)
        {
            for (auto& gpuInfo : cache)
            {
                if (IsEqualLUID(gpuInfo.luid, res.luid))
                {
                    gpuInfo.usesVkd3dProton = res.usesVkd3dProton;
                    gpuInfo.fsr4Capable = res.fsr4Capable;
                    // Ray Regen (FFX-MLD) shares the RDNA4 + fp8/WMMA requirement with FSR4; gate on
                    // the same capability for now. Refine if a stricter MLD 1.1.0 check is needed.
                    gpuInfo.rayRegenCapable = res.fsr4Capable;
                    break;
                }
            }
        }
    }

    auto detectedGpus = IdentifyGpu::getAllGpus();
    std::string gpus = "Detected GPUs:\n";

    std::string indent(22, ' ');

    for (auto& gpu : detectedGpus)
    {
        gpus += std::format("{}{}\n", indent, gpu.name);
        gpus += std::format("{}    vendorId: {:X}, deviceId: {:X}, VRAM: {}MB\n", indent, (uint32_t) gpu.vendorId,
                            gpu.deviceId, gpu.dedicatedVramInBytes / (1024 * 1024));
        gpus += std::format("{}    dxvk: {}, vkd3d-proton: {}\n", indent, gpu.usesDxvk, gpu.usesVkd3dProton);
        gpus += std::format("{}    Upscaler support - fsr4: {}, dlss: {}\n", indent, gpu.fsr4Capable, gpu.dlssCapable);
    }

    spdlog::info(gpus);
}