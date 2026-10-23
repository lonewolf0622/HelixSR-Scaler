HelixSR Scaler for Linux and Steam Deck
=======================================

Optional DirectX 12 Neural Rendering: see HelixSR-NR/INSTALL.txt. It is off by
default and requires a private model cache prepared from your local NR DLL.

HelixSR Scaler lets HelixSR run in games that only have DLSS, XeSS or FSR 2.
It is OptiScaler with HelixSR built in: HelixSR is one more choice in OptiScaler's upscaler list,
and everything else OptiScaler does is still there.

Games run it through Proton (Steam) or Wine (Heroic, Lutris), so these are the same files as the
Windows version, with a setup made for Linux.

Don't use it in online games or games with anti-cheat.


BEFORE YOU START

Set up HelixSR first: run helixsr-setup.sh from the HelixSR download (or use the HelixSR app).
That builds HelixSR's network into its amd_fidelityfx_dx12.dll. Leave the HelixSR folder in
Downloads (or your home folder) and HelixSR Scaler's setup finds it by itself.


THE EASY WAY

The HelixSR Desktop app (https://github.com/lonewolf0622/HelixSR-Desktop-) sets up HelixSR and
installs HelixSR Scaler into your games with a click: pick a game, choose HelixSR Scaler, Install.
It downloads HelixSR Scaler by itself.


INSTALLING BY HAND

1. Extract this download to its own folder, for example in Downloads.
   (Not into the game's folder.)

2. Open a terminal in that folder and run:

       ./setup_linux.sh

   On Steam Deck: switch to Desktop Mode, open the folder in Dolphin, right-click an empty
   spot > Open Terminal Here.

3. It lists your Steam games that have DLSS, FSR or XeSS. Type the number of the game.
   For Heroic, Lutris or other games, drag the game's folder into the terminal window instead.

4. Installed as dxgi.dll (the usual case), it needs no launch options. If the setup installs it under
   another name, it shows the Steam launch options to use and copies them to the clipboard. In Steam:
   right-click the game > Properties > General > Launch Options, and paste them.
   (Heroic or Lutris: add the environment variable WINEDLLOVERRIDES with the value the setup shows.)

5. Start the game. In its graphics settings, turn on DLSS (or FSR or XeSS if there is no DLSS).

6. Press Insert for the menu. The top of the menu shows ONLINE when HelixSR is running.
   The Steam Deck has no Insert key: in the game's controller settings, set a back button to
   the keyboard key Insert.

Run setup_linux.sh again with a newer HelixSR Scaler or HelixSR to update a game.

The setup puts OptiScaler.dll into the game as dxgi.dll and HelixSR into the HelixSR-fsrfg folder
next to the game's .exe (HelixSR's amd_fidelityfx_dx12.dll and its helixsr-fg folder).


FRAME GENERATION

Frame generation is OptiScaler's, set like in OptiScaler. HelixSR's frame generation is one more
DLSS frame-generation provider (like Nukem's) for DirectX 12 games with DLSS Frame Generation: the
game's own DLSS Frame Generation runs HelixSR's. It is off until you turn it on: the setup asks when
HelixSR's frame generation was found, and the HelixSR app asks too. Later, in the menu (Insert key):
Frame Generation, pick HelixSR (or AMD FSR, Intel XeSS, Off), then restart the game. Or set these
under [FrameGen] in OptiScaler.ini:

    FGInput=nvngxfg
    FGNvngxReplacement=HelixSR

Then turn on DLSS Frame Generation in the game's graphics settings.


IN THE MENU (Insert key)

In the HELIXSR CONFIG panel at the top (pick HelixSR in the UPSCALER panel next to it):
- Speed boost: faster int8 math. Auto lets HelixSR decide by the card; On can help cards it leaves
  off (e.g. GTX 16xx). Turn it off if the picture shimmers.
- GPU mode: Auto is right for most cards.
- Frame gen: On makes HelixSR the frame-generation provider (FGInput=nvngxfg,
  FGNvngxReplacement=HelixSR in OptiScaler.ini); Off sets those back to auto.
- The STATUS panel shows if HelixSR runs, and opens HelixSR's folder (helixsr.log tells what it did).
Under them is everything from OptiScaler, its sharpening (RCAS) too. With HelixSR as the
frame-generation provider, its quality (Light / Full, QuickFlow) is under Frame Generation.

Changes are saved in OptiScaler.ini in the game's folder ([HelixSR]). It is the only settings
file, and you can edit it with a text editor too. Restart the game after changing it.
(HelixSR Scaler copies HelixSR's part to HelixSR-fsrfg/helixsr.ini when the game starts; leave that file be.)


IF SOMETHING'S WRONG

- No menu when you press Insert: the launch options are missing or mistyped. Check step 4.
- The game doesn't start: run setup_linux.sh again with --name winmm.dll (or version.dll), and
  change the launch options to match (winmm=n,b).
- RCAS, Output Scaling or Reactive Mask errors in OptiScaler.log: turn on Precompiled Shaders in
  the menu, or install d3dcompiler_47 with protontricks.


REMOVING IT

Run HelixSR-Scaler-remove.sh in the game's folder (the folder with dxgi.dll), or run
./setup_linux.sh --remove and pick the game. Files the game had before are put back.
Then take WINEDLLOVERRIDES out of the launch options.


REPORTING A PROBLEM

Open an issue at https://github.com/lonewolf0622/HelixSR-Scaler/issues and attach
HelixSR-fsrfg/helixsr.log and OptiScaler.log from the game's folder.


HelixSR Scaler is based on OptiScaler (https://github.com/optiscaler/OptiScaler) and, like it,
is free software under the GPL-3.0 license (Licenses/OptiScaler_LICENSE.txt). Source code:
https://github.com/lonewolf0622/HelixSR-Scaler
