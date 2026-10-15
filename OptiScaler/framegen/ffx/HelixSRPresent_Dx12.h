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
// NVSDK_NGX_D3D12_EvaluateFeature, NVIDIA's core reads the parameters). HelixSR's own presenting swapchain
// (HelixSRSwapchain_Dx12, adapted from the FidelityFX one, up to 3 generated frames per real frame) presents and paces
// the real and the generated frames; its frameGenerationCallback calls Generate() instead of FSR's interpolation.
namespace HelixSRPresent
{
// [HelixSR] FGPresent = optiscaler (the game's DLSS FG inputs, OptiScaler's FSR FG swapchain, HelixSR generates)
bool Requested();

// Requested() and the route is active: FGInput = dlssg, FGOutput = fsrfg, HelixSR as the NGX frame generation
bool Active();

// The FSR FG output runs HelixSR's frame generation (the route is configured): name it "HelixSR FG" in the UI
bool Labeled();

// The game's Streamline data, kept as given (Streamline_Inputs / Streamline_Hooks)
void StoreConstants(int fIndex, const sl::Constants& constants);
void StoreOptions(const sl::DLSSGOptions& options); // only version 1 fields are read

// NGX init of HelixSR's frame generation (idempotent), when the FG output is created
void Init(ID3D12Device* device);

constexpr uint32_t kMaxGenerated = 3; // DLSS 4's 4x

struct Frame
{
    ID3D12GraphicsCommandList* cmdList = nullptr;       // the swapchain's command list for this real frame
    ID3D12Resource* backbuffer = nullptr;               // presentColor: the real frame with the HUD
    D3D12_RESOURCE_STATES backbufferState {};           //
    ID3D12Resource* outputs[kMaxGenerated] {};          // outputs[0 .. count - 1]: the generated frames, in present
    D3D12_RESOURCE_STATES outputStates[kMaxGenerated] {}; // order (output i is at time (i + 1) / (count + 1))
    uint32_t count = 1;                                 // generated frames per real frame (DLSSG.MultiFrameCount)
    uint64_t frameID = 0;                               // frame id (fIndex = frameID % BUFFER_COUNT)
    bool reset = false;                                 // the swapchain's reset (a frame id gap)
};

// The most generated frames per real frame HelixSR's frame generation offers (1 - kMaxGenerated)
uint32_t MaxGenerated();

// Records HelixSR's generation of frame.outputs[0 .. count - 1] on frame.cmdList: one NGX evaluation per generated
// frame (MultiFrameIndex 1 .. count), all on this one command list. generated[i]: output i was written by HelixSR;
// for the others the caller shows the real frame in that slot. False: nothing was generated.
bool Generate(IFGFeature_Dx12* fg, const Frame& frame, bool generated[kMaxGenerated]);

// Release the NGX feature and owned resources (FG context destroyed)
void Release();
} // namespace HelixSRPresent
