HelixSR Scaler 1.1.0

New in 1.1.0: HelixSR Scaler is OptiScaler first. Without HelixSR it behaves like OptiScaler.
With HelixSR installed (the HelixSR-fsrfg folder next to the game's .exe), HelixSR is the upscaler in
DirectX 12, DirectX 11 and Vulkan games, with AMD FSR 3 / FSR 4 and Intel XeSS still in the list.
HelixSR frame generation works in every DirectX 12 game. It is off until you turn it on: the HelixSR
app asks, or pick HelixSR under Frame Generation in the menu (Insert key) and restart the game.
In the menu, Frame Generation offers HelixSR, AMD FSR, Intel XeSS or Off.

What goes into the game (the HelixSR app does this for you):
- OptiScaler.dll, renamed to dxgi.dll, with OptiScaler.ini and the OptiScaler folder, next to the
  game's .exe
- HelixSR-fsrfg next to the game's .exe: HelixSR's amd_fidelityfx_dx12.dll, and its helixsr-fg
  folder for frame generation

Select DLSS (or FSR / XeSS if DLSS is unavailable) in the game's graphics settings.
Press Insert to open the menu. Edit OptiScaler.ini to change the other settings ([HelixSR] has
HelixSR's own; FrameGeneration=on switches HelixSR frame generation on).

For installation by hand, back up matching files, copy this folder next to the game's
executable and rename OptiScaler.dll to dxgi.dll. Make a folder HelixSR-fsrfg there and copy
HelixSR's built amd_fidelityfx_dx12.dll and its helixsr-fg folder into it. setup_windows.bat
does these steps. Games which do not load dxgi.dll may need another supported proxy name and
the corresponding Proton DLL override.

HelixSR Scaler is based on OptiScaler and is licensed under GPL-3.0.
Source: https://github.com/lonewolf0622/HelixSR-Scaler
See Licenses for third-party notices. The separate HelixSR DLL has its own license.
