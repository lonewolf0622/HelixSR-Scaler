#pragma once
#include "SysUtils.h"

#include <d3d12.h>

class IFGFeature_Dx12;

namespace sl
{
struct Constants;
struct DLSSGOptions;
} // namespace sl

// HelixSR Scaler: HelixSR's frame generation presented by OptiScaler's FSR FG swapchain ([HelixSR] FGPresent =
// optiscaler, see HELIXSR_FG_PRESENT.md). The game's DLSS Frame Generation inputs (Streamline tags, constants and
// options, FGInput = dlssg) are put into an NGX DLSS-G parameter object with the keys and value types Streamline's
// sl.dlss_g uses, and helixsr.dll generates the frame through the same NGX path as before (Nvngx_FG ->
// NVSDK_NGX_D3D12_EvaluateFeature, NVIDIA's core reads the parameters). The FidelityFX swapchain presents and paces
// the real and the generated frames; its frameGenerationCallback calls Generate() instead of FSR's interpolation.
namespace HelixSRPresent
{
// [HelixSR] FGPresent = optiscaler (the game's DLSS FG inputs, OptiScaler's FSR FG swapchain, HelixSR generates)
bool Requested();

// Requested() and the route is active: FGInput = dlssg, FGOutput = fsrfg, HelixSR as the NGX frame generation
bool Active();

// The game's Streamline data, kept as given (Streamline_Inputs / Streamline_Hooks)
void StoreConstants(int fIndex, const sl::Constants& constants);
void StoreOptions(const sl::DLSSGOptions& options); // only version 1 fields are read

// NGX init of HelixSR's frame generation (idempotent), when the FG output is created
void Init(ID3D12Device* device);

struct Frame
{
    ID3D12GraphicsCommandList* cmdList = nullptr; // the FidelityFX swapchain's command list for this generation
    ID3D12Resource* backbuffer = nullptr;         // presentColor: the real frame with the HUD
    D3D12_RESOURCE_STATES backbufferState {};     //
    ID3D12Resource* output = nullptr;             // outputs[0]: the generated frame the swapchain presents
    D3D12_RESOURCE_STATES outputState {};         //
    uint64_t frameID = 0;                         // FidelityFX frame id (fIndex = frameID % BUFFER_COUNT)
    bool reset = false;                           // FidelityFX's reset (a frame id gap)
};

// Records HelixSR's generation of frame.output on frame.cmdList. False: nothing usable was generated (the caller
// shows the real frame instead).
bool Generate(IFGFeature_Dx12* fg, const Frame& frame);

// Release the NGX feature and owned resources (FG context destroyed)
void Release();
} // namespace HelixSRPresent
