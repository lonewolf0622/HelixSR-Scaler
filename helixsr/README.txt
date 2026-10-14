HelixSR Scaler
==============

HelixSR Scaler lets HelixSR run in games that only have DLSS, XeSS or FSR 2.
It is OptiScaler with HelixSR built in: HelixSR is one more choice in OptiScaler's upscaler list,
and everything else OptiScaler does is still there.

On Linux or Steam Deck? Use the Linux download and read its README.txt instead.

Don't use it in online games or games with anti-cheat.


INSTALLING

1. Set up HelixSR first (run helixsr-setup from the HelixSR download). After that, HelixSR's
   amd_fidelityfx_dx12.dll contains the upscaler.

2. In the game's folder (where the game's .exe is), make a folder called HelixSR.
   Copy HelixSR's amd_fidelityfx_dx12.dll into it and rename the copy to helixsr.dll.

3. Copy everything from this download into the game's folder, next to the .exe.

4. Rename OptiScaler.dll to dxgi.dll. If the game doesn't start, try winmm.dll or version.dll instead.
   (setup_windows.bat does this step for you and asks which name to use.)

5. Start the game. In its graphics settings, turn on DLSS (or FSR or XeSS if there is no DLSS).

6. Press Insert. HelixSR is picked in the upscaler list, with its settings under it.


IN THE MENU (Insert key)

Under the upscaler list, while HelixSR is picked:
- Sharpness: how crisp the picture looks. Try Off if edges sparkle or shimmer.
- Speed boost: faster math on graphics cards that support it. Turn it off if the picture shimmers.
- GPU mode: Auto is right for most cards.
- Open the HelixSR folder: helixsr.log in there tells what HelixSR did.
Everything else in the menu is OptiScaler's.

Changes are saved in HelixSR\helixsr.ini. Restart the game after changing them.


REPORTING A PROBLEM

Open an issue at https://github.com/lonewolf0622/HelixSR-Scaler/issues and attach
HelixSR\helixsr.log and OptiScaler.log from the game's folder.


REMOVING IT

Delete what you copied: dxgi.dll (or the name you picked), OptiScaler.ini, the HelixSR folder,
and the other files from this download.


HelixSR Scaler is based on OptiScaler (https://github.com/optiscaler/OptiScaler) and, like it,
is free software under the GPL-3.0 license (Licenses\OptiScaler_LICENSE.txt). Source code:
https://github.com/lonewolf0622/HelixSR-Scaler
