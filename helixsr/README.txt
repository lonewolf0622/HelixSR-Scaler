HelixSR Scaler
==============

HelixSR Scaler lets HelixSR run in games that only have DLSS, XeSS or FSR 2.
It is OptiScaler with HelixSR built in: HelixSR is one more choice in OptiScaler's upscaler list,
and everything else OptiScaler does is still there.

On Linux or Steam Deck? Use the Linux download and read its README.txt instead.

Don't use it in online games or games with anti-cheat.


THE EASY WAY

The HelixSR Desktop app (https://github.com/lonewolf0622/HelixSR-Desktop-) sets up HelixSR and
installs HelixSR Scaler into your games with a click: pick a game, choose HelixSR Scaler, Install.
It downloads HelixSR Scaler by itself.


INSTALLING BY HAND

1. Set up HelixSR first (run helixsr-setup from the HelixSR download). After that, HelixSR's
   amd_fidelityfx_dx12.dll contains the upscaler, and the helixsr-fg folder next to it has
   frame generation.

2. Copy everything from this download into the game's folder, next to the .exe.

3. Run setup_windows.bat there. It asks which name to use for OptiScaler.dll (dxgi.dll is right for
   most games), then finds HelixSR (in your Downloads, or the HelixSR app's) and copies it into the
   game's HelixSR folder, with frame generation. If it can't find HelixSR, drag HelixSR's
   amd_fidelityfx_dx12.dll into its window.

   Without setup_windows.bat: in the game's folder, make a folder called HelixSR. Copy HelixSR's
   amd_fidelityfx_dx12.dll into it and rename the copy to helixsr.dll. Copy HelixSR's helixsr-fg
   folder into it too (HelixSR\helixsr-fg): without it there is no frame generation. Then rename
   OptiScaler.dll to dxgi.dll (if the game doesn't start, try winmm.dll or version.dll instead).

4. Start the game. In its graphics settings, turn on DLSS (or FSR or XeSS if there is no DLSS),
   and DLSS Frame Generation if the game has it.

5. Press Insert. The top of the menu shows ONLINE when HelixSR is running.


FRAME GENERATION

HelixSR's frame generation runs when the game's DLSS Frame Generation is turned on. HelixSR Scaler
sets that up by itself when it finds the game's sl.dlss_g.dll. A game with an older Streamline (NVIDIA's
DLSS Frame Generation files) uses the newer ones in OptiScaler\streamline that come with this download. If the game has DLSS Frame Generation
but OptiScaler.log says no sl.dlss_g.dll was found, set FGInput=nvngxfg under [FrameGen] in
OptiScaler.ini. To switch HelixSR's frame generation off, set FrameGeneration=off under [HelixSR].


IN THE MENU (Insert key)

In the HELIXSR CONFIG panel at the top (pick HelixSR in the UPSCALER panel next to it):
- Speed boost: faster int8 math. Auto lets HelixSR decide by the card; On can help cards it leaves
  off (e.g. GTX 16xx). Turn it off if the picture shimmers.
- GPU mode: Auto is right for most cards.
- The STATUS panel shows if HelixSR runs, and opens the HelixSR folder (helixsr.log tells what it did).
Under them is everything from OptiScaler, its sharpening (RCAS) too.

Changes are saved in OptiScaler.ini in the game's folder ([HelixSR]). It is the only settings
file, and you can edit it with a text editor too. Restart the game after changing it.
(HelixSR Scaler copies HelixSR's part to HelixSR\helixsr.ini when the game starts; leave that file be.)


REPORTING A PROBLEM

Open an issue at https://github.com/lonewolf0622/HelixSR-Scaler/issues and attach
HelixSR\helixsr.log and OptiScaler.log from the game's folder.


REMOVING IT

Delete what you copied: dxgi.dll (or the name you picked), OptiScaler.ini, the HelixSR folder,
and the other files from this download.


HelixSR Scaler is based on OptiScaler (https://github.com/optiscaler/OptiScaler) and, like it,
is free software under the GPL-3.0 license (Licenses\OptiScaler_LICENSE.txt). Source code:
https://github.com/lonewolf0622/HelixSR-Scaler
