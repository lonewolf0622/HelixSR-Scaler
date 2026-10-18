#pragma once

// HelixSR Scaler: what HelixSR.cpp shares with the HUD (HelixSRHud.cpp). Not for OptiScaler's files.

#include <filesystem>
#include <string>

#include <imgui/imgui.h>

#include "OptiTypes.h"

namespace HelixSR::detail
{
// helixsr.ini, as the menu shows it
struct Settings
{
    int int8 = 0;     // Auto, On, Off
    int wave = 0;     // Auto, 64, 32
    int frameGen = 0; // On, Off (HelixSR's frame generation)
};

// The last start of the upscaler, from helixsr.log
struct LogStatus
{
    std::string version; // "1.9.0"
    std::string int8;    // "on" / "off"
    int networkWave = 0; // 32 / 64
    bool networkMissing = false;
    bool gpuUnsupported = false;
    bool networkFailed = false;
    bool fgUnsupported = false; // "frame generation NOT available on this GPU" (no 32-lane waves: Vega, Polaris)
};

struct MenuState
{
    LogStatus status;
    Settings settings; // what the menu shows
    Settings saved;    // what helixsr.ini has
    Settings initial;  // what the game started with
    double lastRead = -10.0;
    bool restartNeeded = false;
    bool saveFailed = false;
};

extern const char* const kInt8[];
inline constexpr int kInt8Count = 3;
inline constexpr const char* kInt8Help =
    "Faster int8 math. Auto lets HelixSR decide by the graphics card. On can help cards it leaves off that have fast "
    "int8 (e.g. GTX 16xx). If the picture shimmers or flickers, try Off. Restart the game after changing it.";
extern const char* const kWave[];
inline constexpr int kWaveCount = 3;
extern const char* const kFrameGen[];
inline constexpr int kFrameGenCount = 2;
inline constexpr const char* kFrameGenHelp =
    "HelixSR's frame generation: the game's DLSS Frame Generation runs HelixSR's. Off if the game crashes or "
    "stutters with it. Restart the game after changing it.";
bool FrameGenInstalled(); // the HelixSR folder has frame generation, switched on or off

inline constexpr const char* kWaveHelp = "How many threads HelixSR runs together (wave size). Auto (32) is right for "
                                         "most cards. For AMD RX 7000 / 9000 with a broken picture, try 64.";

extern std::filesystem::path folder;
extern bool found;

MenuState& Menu();
void RefreshMenu();           // reads helixsr.log (and helixsr.ini the first time) at most every two seconds
void SaveMenuSettings();      // writes Menu().settings to helixsr.ini
::Upscaler Backend();         // the configured upscaler of the game's API
::Upscaler HelixSRUpscaler(); // DLSS, or DLSS w/Dx12 in DirectX 11 games
void ChooseHelixSR();
void ChooseBackend(::Upscaler upscaler);
void OpenFolder();
void ApplyFsrDllDefaults(); // HelixSRFsrDll.cpp: HelixSR's FidelityFX DLL as the FFX upscaler (no helixsr.dll)
} // namespace HelixSR::detail
