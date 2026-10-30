#pragma once
#include "DLSSFeature.h"
#include <upscalers/IFeature_Dx12.h>
#include <shaders/rcas/RCAS_Dx12.h>
#include <string>

class DLSSFeatureDx12 : public DLSSFeature, public IFeature_Dx12
{
  private:
    // HelixSR Scaler: Preset K's DLL for this upscaler (the DLSS Upscaler list), picked when it is created
    PFN_D3D12_CreateFeature _create = nullptr;
    PFN_D3D12_EvaluateFeature _evaluate = nullptr;
    PFN_D3D12_ReleaseFeature _release = nullptr;

  protected:
    bool InitDLSS(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);

  public:
    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

    void Shutdown(ID3D12Device* InDevice);

    feature_version Version() override { return DLSSFeature::Version(); }
    Upscaler GetUpscalerType() const final { return DLSSFeature::GetUpscalerType(); }
    API Api() const override { return IFeature_Dx12::Api(); }
    bool CallsUpscalerEndByItself() override { return IFeature_Dx12::CallsUpscalerEndByItself(); }
    std::optional<double> ReadUpscalerTime(void* commandQueue) override
    {
        return IFeature_Dx12::ReadUpscalerTime(commandQueue);
    };
    void ReadDetailedGpuTimes(void* commandQueue, std::vector<DetailedGpuTime>& detailedGpuTimes) override
    {
        return IFeature_Dx12::ReadDetailedGpuTimes(commandQueue, detailedGpuTimes);
    };

    bool IsWithDx12() override { return false; }

    DLSSFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~DLSSFeatureDx12();
};
