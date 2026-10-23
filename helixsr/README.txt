HelixSR Scaler 1.1.0

Optional DirectX 12 Neural Rendering: see HelixSR-NR/INSTALL.txt. It is off by
default and requires a private model cache prepared from your local NR DLL.

New in 1.1.0: HelixSR Scaler is OptiScaler first and behaves like OptiScaler. With HelixSR
installed (the HelixSR-fsrfg folder next to the game's .exe), HelixSR is the upscaler in DirectX 12,
DirectX 11 and Vulkan games, with AMD FSR 3 / FSR 4 and Intel XeSS still in the list. Frame
generation is OptiScaler's, set in OptiScaler.ini or its menu like in OptiScaler; HelixSR's frame
generation is one more DLSS frame-generation provider (like Nukem's) for DirectX 12 games with DLSS
Frame Generation. It is off until you turn it on: the HelixSR app or the setup asks, or pick HelixSR
under Frame Generation in the menu (Insert key) and restart the game.

OptiScaler.ini lines for HelixSR (the app and the setup write them for you):
- Upscaling (found by itself when HelixSR-fsrfg is next to the game's .exe):
    [Upscalers] Dx12Upscaler=ffx, Dx11Upscaler=ffx_12, VulkanUpscaler=ffx_12
    [Libraries] FfxDx12SRPath=<the game folder>\HelixSR-fsrfg\amd_fidelityfx_dx12.dll
- HelixSR frame generation (DirectX 12 games with DLSS Frame Generation; turn that on in the game):
    [FrameGen] FGInput=nvngxfg, FGNvngxReplacement=HelixSR
- No frame generation: [FrameGen] FGInput=auto (or nofg)

What goes into the game (the HelixSR app does this for you):
- OptiScaler.dll, renamed to dxgi.dll, with OptiScaler.ini and the OptiScaler folder, next to the
  game's .exe
- HelixSR-fsrfg next to the game's .exe: HelixSR's amd_fidelityfx_dx12.dll, and its helixsr-fg
  folder for frame generation

Select DLSS (or FSR / XeSS if DLSS is unavailable) in the game's graphics settings.
Press Insert to open the menu. Edit OptiScaler.ini to change the other settings ([HelixSR] has
HelixSR's own: Int8, WaveSize, and FGPreset / QuickFlow for its frame generation).

For installation by hand, back up matching files, copy this folder next to the game's
executable and rename OptiScaler.dll to dxgi.dll. Make a folder HelixSR-fsrfg there and copy
HelixSR's built amd_fidelityfx_dx12.dll and its helixsr-fg folder into it. setup_windows.bat
does these steps. Games which do not load dxgi.dll may need another supported proxy name and
the corresponding Proton DLL override.

HelixSR Scaler is based on OptiScaler and is licensed under GPL-3.0.
Source: https://github.com/lonewolf0622/HelixSR-Scaler
See Licenses for third-party notices. The separate HelixSR DLL has its own license.
