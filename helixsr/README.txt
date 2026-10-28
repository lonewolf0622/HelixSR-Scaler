HelixSR Scaler 2.1.0

HelixSR Scaler is OptiScaler with HelixSR's upscalers in its list:
- HLX DLSS: HelixSR's DLSS upscaler (the HelixSR folder next to the game's .exe, helixsr.dll).
  It is the DLSS upscaler on GPUs without DLSS, in DirectX 12, DirectX 11 and Vulkan games.
- HLX FSR 4: HelixSR's own FSR 4-class upscaler, AMD's FSR 4 network on HelixSR's own pipeline
  (OptiScaler\amd_fidelityfx_upscaler_dx12.dll; AMD's own FSR is next to it as
  amd_fidelityfx_upscaler_dx12.amd.dll). It is an upscaler of its own in the list, next to AMD's FSR.
AMD FSR 3 / FSR 4 (with the FSR 4 presets and models on GPUs that have FSR 4) and Intel XeSS stay in the list, and frame generation is OptiScaler's (AMD FSR or
XeSS frame generation), set in its menu or OptiScaler.ini like in OptiScaler.

First set HelixSR up: run helixsr-setup.bat from the HelixSR 3.1 download (or use the HelixSR app).
It builds HelixSR on your PC. HelixSR 3.0 and 3.1 need HelixSR Scaler 2.0.0 or newer.

Then, for each game:
- this download's setup_windows.bat: extract this download into the game's folder (next to its .exe;
  Unreal Engine games: Binaries\Win64) and run setup_windows.bat there. It asks for the file name
  (dxgi.dll is the usual one), finds your HelixSR by itself and copies it in (HLX DLSS, and HLX FSR 4
  with AMD's FSR kept next to it). To remove it: Remove_OptiScaler.bat in the game's folder.
- or HelixSR's setup / the HelixSR app: they list your games and install, update or remove it.

What goes into the game: OptiScaler.dll as dxgi.dll (or another proxy name the setup tells you),
OptiScaler.ini and the OptiScaler folder next to the game's .exe, and the HelixSR folder with
HelixSR's built helixsr.dll.

Select DLSS (or FSR / XeSS if DLSS is unavailable) in the game's graphics settings. Press Insert
to open the menu and pick the upscaler there. HelixSR's own settings (Int8, WaveSize) are under
[HelixSR] in OptiScaler.ini; HelixSR Scaler copies them to HelixSR\helixsr.ini when the game starts.

Problems: https://github.com/lonewolf0622/HelixSR-Scaler/issues (attach OptiScaler.log and
HelixSR\helixsr.log from the game's folder).

HelixSR Scaler is based on OptiScaler (https://github.com/optiscaler/OptiScaler) and, like it,
is free software under the GPL-3.0 license (Licenses/OptiScaler_LICENSE.txt). Source code:
https://github.com/lonewolf0622/HelixSR-Scaler
OptiScaler/plugins/OptiPatcher.asi is OptiPatcher (MIT, Licenses/OptiPatcher_LICENSE.txt).
The separate HelixSR DLLs (HelixSR\helixsr.dll, HLX FSR 4) have their own license.
