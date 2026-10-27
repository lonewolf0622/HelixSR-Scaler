HelixSR Scaler 2.0.0 for Linux and Steam Deck
=============================================

HelixSR Scaler is OptiScaler with HelixSR's upscalers in its list: HLX DLSS (HelixSR's DLSS
upscaler, also for DirectX 11 and Vulkan games) and HLX FSR 4 (AMD's FSR 4 network on HelixSR's own
pipeline). AMD FSR 3 / FSR 4 and Intel XeSS stay in the list; frame generation is OptiScaler's (AMD
FSR or XeSS frame generation). Games run it through Proton (Steam) or Wine (Heroic, Lutris): these
are the same files as on Windows.

Don't use it in online games or games with anti-cheat.


INSTALLING

Run ./helixsr-setup.sh from the HelixSR 3.0 download (on Steam Deck: Desktop Mode, open the folder
in Dolphin, right-click an empty spot > Open Terminal Here). It builds HelixSR on your PC, lists
your games and installs HelixSR Scaler into the ones you answer s for. Installed as dxgi.dll (the
usual case) it needs no launch options; otherwise the setup shows the Steam launch options to use.
(Heroic or Lutris: add the environment variable WINEDLLOVERRIDES with the value the setup shows.)

Start the game. In its graphics settings, turn on DLSS (or FSR or XeSS if there is no DLSS).
Press Insert for the menu. The Steam Deck has no Insert key: in the game's controller settings,
set a back button to the keyboard key Insert.

Run the setup of a newer HelixSR to update a game, or helixsr-uninstall.sh to remove it (the
game's own files are put back).


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
