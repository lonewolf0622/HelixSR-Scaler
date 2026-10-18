#pragma once
#include "Nvngx_DllProxy.h"

// HelixSR Scaler: HelixSR's frame generation as a DLSSG provider ([FrameGen] FGNvngxReplacement = HelixSR), like
// Nukem's: HelixSR's FidelityFX DLL (HelixSR-fsrfg\amd_fidelityfx_dx12.dll with its helixsr-fg folder), or an older
// install's HelixSR\helixsr.dll. DirectX 12 only.
class Nvngx_HelixSR : public Nvngx_DllProxy
{
  protected:
    void LoadLibraries() override final;

  public:
    Nvngx_HelixSR() { LoadLibraries(); }

    int getMaxFakeFramesCount() override { return 1; }
    FGNvngxReplacement getType() override { return FGNvngxReplacement::HelixSR; }
};
