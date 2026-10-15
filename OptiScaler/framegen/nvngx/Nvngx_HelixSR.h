#pragma once
#include "Nvngx_DllProxy.h"

// HelixSR's frame generation as the DLSSG provider: NVIDIA's DLSS Frame Generation network run by HelixSR on any GPU
// (helixsr.dll in OptiScaler's folder, with its helixsr-fg folder next to it)
class Nvngx_HelixSR : public Nvngx_DllProxy
{
  protected:
    void LoadLibraries() override final;

    // helixsr.dll's limit of generated frames per real frame (helixsr.ini [FrameGeneration] MultiFrame, and what its
    // frame generation can do on this PC); older helixsr.dll builds without it generate one
    typedef int (*PFN_HelixSR_FgMaxGeneratedFrames)();
    PFN_HelixSR_FgMaxGeneratedFrames _maxGeneratedFrames = nullptr;

  public:
    Nvngx_HelixSR() { LoadLibraries(); }

    int getMaxFakeFramesCount() override;
    FGNvngxReplacement getType() override { return FGNvngxReplacement::HelixSR; }
};
