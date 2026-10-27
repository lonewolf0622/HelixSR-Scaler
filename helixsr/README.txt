HelixSR Scaler 2.0.0

HelixSR Scaler is OptiScaler with HelixSR's upscalers in its list:
- HLX DLSS: HelixSR's DLSS upscaler (the HelixSR folder next to the game's .exe, helixsr.dll).
  It is the DLSS upscaler on GPUs without DLSS, in DirectX 12, DirectX 11 and Vulkan games.
- HLX FSR 4: AMD's FSR 4 network on HelixSR's own pipeline
  (OptiScaler\amd_fidelityfx_upscaler_dx12.dll; AMD's own FSR is next to it as
  amd_fidelityfx_upscaler_dx12.amd.dll and stays in the list).
AMD FSR 3 / FSR 4 and Intel XeSS stay in the list, and frame generation is OptiScaler's (AMD FSR or
XeSS frame generation), set in its menu or OptiScaler.ini like in OptiScaler.

Install it with HelixSR's setup (helixsr-setup.bat / helixsr-setup.sh in the HelixSR 3.0 download):
it builds HelixSR on your PC, then installs HelixSR Scaler into the games you pick (answer s) and
updates or removes it. HelixSR 3.0 needs HelixSR Scaler 2.0.0 or newer.

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
