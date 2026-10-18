HelixSR Scaler 1.0.7

New in 1.0.7: DirectX 11 games get HelixSR too (pick DLSS in the game; OptiScaler runs it through its
DirectX 12 interop, and falls back to FSR 2.2 if HelixSR does not start). Vulkan games through OptiScaler
have an optional setting, [HelixSR] VulkanComputeQueue (see OptiScaler.ini). It is off by default.

HelixSR frame generation (DirectX 12 games) runs when HelixSR's helixsr-fg folder is installed next
to HelixSR's DLL. HelixSR Scaler picks the route by the game: the game's own DLSS Frame Generation,
else the game's FSR 3.1 / XeSS frame generation or the upscaler, presented by Scaler's Streamline.
In the menu (Insert key, Frame Generation) pick HelixSR, AMD FSR, Intel XeSS or Off; restart the game
after changing it. FG Input, FG Output and the provider stay available there for other combinations.
DirectX 11 and Vulkan games get no HelixSR frame generation.

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
