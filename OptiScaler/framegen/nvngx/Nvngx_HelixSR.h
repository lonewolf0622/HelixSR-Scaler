#pragma once
#include "Nvngx_DllProxy.h"

// HelixSR Scaler: HelixSR's frame generation as a DLSSG provider ([FrameGen] FGNvngxReplacement = HelixSR), like
// Nukem's: dedicated HelixSR-fsrfg\helixsr_fg.dll with its helixsr-fg dependencies.
// Legacy combined DLLs remain compatible when the dedicated DLL is absent. DirectX 12 only.
class Nvngx_HelixSR : public Nvngx_DllProxy
{
  protected:
    void LoadLibraries() override final;

  public:
    Nvngx_HelixSR() { LoadLibraries(); }

    int getMaxFakeFramesCount() override { return 1; }
    FGNvngxReplacement getType() override { return FGNvngxReplacement::HelixSR; }
};
