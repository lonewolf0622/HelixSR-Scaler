#pragma once
#include "Nvngx_DllProxy.h"

// HelixSR's frame generation as the DLSSG provider: NVIDIA's DLSS Frame Generation network run by HelixSR on any GPU
// (helixsr_fg.dll in OptiScaler's folder, with its helixsr-fg folder next to it)
class Nvngx_HelixSR : public Nvngx_DllProxy
{
  protected:
    void LoadLibraries() override final;

  public:
    Nvngx_HelixSR() { LoadLibraries(); }

    int getMaxFakeFramesCount() override { return 1; }
    FGNvngxReplacement getType() override { return FGNvngxReplacement::HelixSR; }
};
