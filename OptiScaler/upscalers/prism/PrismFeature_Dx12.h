#pragma once

#include <upscalers/IFeature_Dx12.h>

#include <memory>

// Arm Neural Super Sampling (NSS v1) through the Prism runtime. Prism runs Vulkan compute, so this backend needs
// vkd3d-proton: it records into the game's D3D12 command list through vkd3d-proton's Vulkan interop. Built only
// with Prism (OPTISCALER_PRISM, set when the PrismDir MSBuild property points at a Prism build).
class PrismFeatureDx12 : public IFeature_Dx12
{
  public:
    feature_version Version() override { return feature_version { 0, 1, 0 }; }
    Upscaler GetUpscalerType() const final { return Upscaler::Prism; }
    API Api() const override { return IFeature_Dx12::Api(); }
    bool IsWithDx12() final { return false; }

    PrismFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~PrismFeatureDx12();

    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
