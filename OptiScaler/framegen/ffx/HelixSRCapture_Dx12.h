#pragma once
#include "SysUtils.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <string>

namespace sl
{
struct Constants;
} // namespace sl

// HelixSR Scaler: debug capture of the frame-generation inputs ([FrameGen] HelixSRDebugCapture = N, off by default).
// For the frames N .. N+3 (presents of the FSR FG output with frame generation running) it keeps, in the same frames:
//  A  the game's Streamline inputs as received: every tag (depth, motion vectors, HUD-less, UI, distortion) copied
//     at tagging (on the tag's command list) or, for tags without a direct command list, at present; the
//     swapchain's back buffer at present; the sl::Constants as given and the values OptiScaler made of them
//  B  what the FidelityFX frame-generation DLL is given: the depth and motion vectors of the prepare (copied on the
//     prepare's command list), the HUD-less colour, UI, present colour of the generation (copied on the callback's
//     command list) and the generated frame; every field of the prepare, configure, UI registration and dispatch
// GPU copies into read-back buffers; the files are written on a worker thread a few frames after the last capture
// frame, once a fence says the GPU is past it (no waits on the game's threads). At most about 2 GB.
// Output: <game exe folder>\helixsr-capture\<time>\capture.txt (one line per event and file) and the .bin files
// (tightly packed rows of subresource 0 in the footprint format the text names).
namespace HelixSRCapture
{
bool Enabled();

// The present of the FSR FG output (before its dispatch): advances the frame counter (only for presents with frame
// generation running on the game's depth and motion vectors, so menus and loading screens do not count), copies the
// back buffer and the tags that waited for the present, writes the files when they are ready.
void OnPresent(ID3D12CommandQueue* queue, IDXGISwapChain* swapChain, bool generating);

// Group (frame number) of capture points around the dispatch of the current present, and of tags being received
// for the next present; -1 when outside the capture window.
int DispatchGroup();
int TagGroup();

// Where a tag comes from (Streamline hook), for the next Tag() calls on this thread
struct Source
{
    Source(const char* name, uint32_t viewport, int feature = -1);
    ~Source();
};

// A Streamline tag as received. slot / accepted: where OptiScaler stored it and whether it took it.
void Tag(uint32_t slType, int lifecycle, ID3D12Resource* resource, uint32_t state, uint32_t left, uint32_t top,
         uint64_t width, uint32_t height, uint32_t slFrame, ID3D12GraphicsCommandList* cmdList, int slot,
         bool accepted);

// The constants as given and the values OptiScaler made of them (near, far, vertical FOV, aspect, MV scale)
void Constants(const sl::Constants& given, uint32_t slFrame, int slot, float nearOut, float farOut, float fovOut,
               float aspectOut, float mvScaleX, float mvScaleY, bool infinite);

// A GPU copy of a resource for group 'group' (point "B-prepare" etc.); state: the resource's state on that list
void Copy(int group, const char* point, const char* name, ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
          D3D12_RESOURCE_STATES state, const std::string& extra = {});

// A text line of group 'group'
void Line(int group, const std::string& text);

// The HUD-less colour and UI given for a frame ID at configure (the generation of that frame ID reads them)
void NoteConfigure(uint64_t frameID, ID3D12Resource* hudless, D3D12_RESOURCE_STATES hudlessState, ID3D12Resource* ui,
                   D3D12_RESOURCE_STATES uiState);
bool ConfiguredFor(uint64_t frameID, ID3D12Resource** hudless, D3D12_RESOURCE_STATES* hudlessState, ID3D12Resource** ui,
                   D3D12_RESOURCE_STATES* uiState, int* group);

// Text of a resource (pointer, size, format)
std::string ResText(ID3D12Resource* resource);
} // namespace HelixSRCapture
