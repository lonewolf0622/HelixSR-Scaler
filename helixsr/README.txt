HelixSR Scaler 1.0.5

This release provides upscaling only. HelixSR frame generation is not included.
The HelixSR frame-generation route stays disabled even if an older configuration
or helixsr-fg folder remains in the game.

The HelixSR package setup installs this program alongside HelixSR's upscaling DLL.
Select DLSS (or FSR / XeSS if DLSS is unavailable) in the game's graphics settings.
Press Insert to open the menu. Edit OptiScaler.ini to change the other settings.

For installation by hand, back up matching files before copying this folder next
to the game's executable, rename OptiScaler.dll to dxgi.dll, and place HelixSR's
built upscaling DLL in HelixSR/helixsr.dll. Games which do not load dxgi.dll may
need another supported proxy name and the corresponding Proton DLL override.
The HelixSR package setup handles this selection and records backups for removal.

HelixSR Scaler is based on OptiScaler and is licensed under GPL-3.0.
Source: https://github.com/lonewolf0622/HelixSR-Scaler
See Licenses for third-party notices. The separate HelixSR DLL has its own license.
