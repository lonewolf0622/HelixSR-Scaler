#pragma once

#include <d3d12.h>
#include <nvsdk_ngx_params.h>
#include <memory>
#include <string>

namespace HelixSR::Nr
{
// Called after successful command-list Reset, independently of root tracking.
void ResetRecording(ID3D12GraphicsCommandList* commands);
void Collect();
std::string Status();
void RenderSettings();

class Pass
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    Pass();
    ~Pass();
    Pass(const Pass&) = delete;
    Pass& operator=(const Pass&) = delete;
    void Suspend();
    // Returns a scratch target only once the model and submission hooks are ready.
    ID3D12Resource* Prepare(ID3D12Device* device, ID3D12GraphicsCommandList* commands, ID3D12Resource* output);
    bool Dispatch(ID3D12GraphicsCommandList* commands, ID3D12Resource* input, ID3D12Resource* output,
                  ID3D12Resource* depth, ID3D12Resource* motion, NVSDK_NGX_Parameter* parameters,
                  bool hdr, bool invertedDepth, bool lowResMotion, uint32_t renderWidth, uint32_t renderHeight);
};
} // namespace HelixSR::Nr
