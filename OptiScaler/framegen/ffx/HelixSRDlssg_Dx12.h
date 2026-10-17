#pragma once
#include "SysUtils.h"
#include "helixsr_ffx_dlssg.h"

#include <d3d12.h>

class IFGFeature_Dx12;

namespace sl
{
struct Constants;
struct ResourceTag;
} // namespace sl

// HelixSR Scaler: the game's DLSS Frame Generation inputs for HelixSR's FidelityFX frame-generation DLL.
// With FGInput = dlssg and FGOutput = fsrfg, OptiScaler converts the game's Streamline data to FSR 3.1's prepare: near
// / far and the FOV recalculated from the projection, no ClipToPrevClip, and its own choice of images (Depth,
// HiResDepth and LinearDepth as one type; the first image valid only at tagging wins over later ones; images of any
// feature's evaluate; the frame slot it dispatches). When the loaded FidelityFX frame-generation DLL exports
// HelixSR_FfxFgDlssgVersion, the FSR FG output chains a HelixSRFfxDlssgInputs description (helixsr_ffx_dlssg.h) to
// every prepare with the frame's inputs as Streamline's DLSS-G would see them when that frame is presented:
//  - the frame: the Streamline frame of the present (the PCL present marker before the swapchain present), else the
//    frame of the constants OptiScaler stored in the dispatched slot;
//  - its sl::Constants exactly as given (before OptiScaler's changes);
//  - its images tagged with slSetTag / slSetTagForFrame for the DLSS-G viewport (slDLSSGSetOptions; any viewport until
//    the game sets options), never images given to slEvaluateFeature (those are another feature's): per type
//    (kBufferTypeDepth, kBufferTypeMotionVectors, kBufferTypeHUDLessColor, kBufferTypeUIColorAndAlpha) the LAST tag of
//    that frame, with its state and extent. A tag valid until present is the game's image; a tag valid only now (or
//    until evaluate) is copied on the tag's command list at tagging (CopyResource, exact), as Streamline does.
// HelixSR's DLL takes them instead of the FSR values and removes the description before the prepare reaches AMD's DLL.
// Any other DLL: nothing is captured, nothing is chained.
namespace HelixSRDlssg
{
// The FSR FG output found HelixSR's DLL (capturing on) or not (off); set at every dispatch
void SetWanted(bool wanted);

// Streamline_Inputs: the game's constants of a Streamline frame (kept as given, before OptiScaler's changes), stored
// also at the FG index OptiScaler uses for it
void StoreConstants(int fIndex, uint32_t slFrame, const sl::Constants& constants);

// Streamline hooks (slSetTag, slSetTagForFrame only): one tag of a Streamline frame
void Tag(uint32_t viewport, uint32_t slFrame, const sl::ResourceTag& tag, void* cmdBuffer);

// slDLSSGSetOptions: the DLSS-G viewport
void SetDlssgViewport(uint32_t viewport);

// The PCL present marker: the Streamline frame being presented
void MarkPresent(uint32_t slFrame);

struct Options
{
    bool hudless = false; // give the HUD-less image (not disabled by FGDisableHudless)
    bool ui = false;      // give the UI image (it is registered for HelixSR's network this frame)
    bool uiPremultiplied = false;
};

// The description for the prepare of frameID (FidelityFX frame ID, OptiScaler's index fIndex). False: none for this
// frame (not the DLSS-G input, no new constants, or no depth / motion vectors tagged for that frame); the prepare then
// goes out as before.
bool Fill(int fIndex, uint64_t frameID, const Options& options, HelixSRFfxDlssgInputs& out);

// FG context destroyed: the stored constants and tags are dropped (the copies are kept for reuse)
void Release();
} // namespace HelixSRDlssg
