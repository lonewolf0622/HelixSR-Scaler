HelixSR Scaler 2.1.0 for Linux and Steam Deck
=============================================

HelixSR Scaler is OptiScaler with HelixSR's upscalers in its list: HLX DLSS (HelixSR's DLSS
upscaler, also for DirectX 11 and Vulkan games) and HLX FSR 4 (HelixSR's own FSR 4-class upscaler,
AMD's FSR 4 network on HelixSR's own pipeline, listed next to AMD's FSR). AMD FSR 3 / FSR 4 (with the
FSR 4 presets and models on GPUs that have FSR 4) and Intel XeSS stay in the list; frame generation is OptiScaler's (AMD
FSR or XeSS frame generation). Games run it through Proton (Steam) or Wine (Heroic, Lutris): these
are the same files as on Windows.


INSTALLING

First set HelixSR up: run ./helixsr-setup.sh from the HelixSR 3.1 download (or use the HelixSR
app). It builds HelixSR on your PC. On Steam Deck: Desktop Mode, open the folder in Dolphin,
right-click an empty spot > Open Terminal Here.

Then run ./setup_linux.sh from this download. It lists your Steam games with DLSS, FSR or XeSS:
type a game's number (or drag a game's folder into the window for Heroic, Lutris and other stores).
It finds your HelixSR by itself and puts HelixSR Scaler, HLX DLSS and HLX FSR 4 into the game.
HelixSR's setup and the HelixSR app can install it too, and each can update or remove what the
other installed. Installed as dxgi.dll (the usual case) it needs no launch options; otherwise it
shows the Steam launch options to use. (Heroic or Lutris: add the environment variable
WINEDLLOVERRIDES with the value it shows.)

Start the game. In its graphics settings, turn on DLSS (or FSR or XeSS if there is no DLSS).
Press Insert for the menu. The Steam Deck has no Insert key: in the game's controller settings,
set a back button to the keyboard key Insert.

To update a game, run setup_linux.sh (or the setup of a newer HelixSR) again. To remove it, run
HelixSR-Scaler-remove.sh in the game's folder (or helixsr-uninstall.sh): the game's own files are
put back.


IN THE MENU (Insert key)

Pick HLX DLSS or HLX FSR 4 (or AMD FSR, XeSS) as the upscaler. Everything else is OptiScaler's,
its sharpening (RCAS) and frame generation too. Changes are saved in OptiScaler.ini in the game's
folder. HelixSR's own settings (Int8, WaveSize) are under [HelixSR] there: edit them with a text
editor and restart the game (HelixSR Scaler copies them to HelixSR/helixsr.ini when the game starts;
helixsr.log next to it tells what HelixSR did).


IF SOMETHING'S WRONG

- No menu when you press Insert: the launch options are missing or mistyped.
- RCAS, Output Scaling or Reactive Mask errors in OptiScaler.log: turn on Precompiled Shaders in
  the menu, or install d3dcompiler_47 with protontricks.
- Report problems at https://github.com/lonewolf0622/HelixSR-Scaler/issues and attach
  OptiScaler.log and HelixSR/helixsr.log from the game's folder.


HelixSR Scaler is based on OptiScaler (https://github.com/optiscaler/OptiScaler) and, like it,
is free software under the GPL-3.0 license (Licenses/OptiScaler_LICENSE.txt). Source code:
https://github.com/lonewolf0622/HelixSR-Scaler
OptiScaler/plugins/OptiPatcher.asi is OptiPatcher (MIT, Licenses/OptiPatcher_LICENSE.txt).
The separate HelixSR DLLs (HelixSR\helixsr.dll, HLX FSR 4) have their own license.
