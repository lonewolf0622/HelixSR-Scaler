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
    int int8 = 0;      // Auto, On, Off
    int wave = 0;      // Auto, 64, 32
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
extern const char* const kWave[];
inline constexpr int kInt8Count = 3;
inline constexpr int kWaveCount = 3;

inline constexpr const char* kInt8Help = "Faster math (int8) on graphics cards that support it. Auto turns it on "
                                         "where it helps. If the picture shimmers or flickers, try Off.";
inline constexpr const char* kWaveHelp = "How many threads HelixSR runs together (wave size). Auto is right for most "
                                         "cards. For AMD RX 7000 / 9000 with a broken picture, try 64.";

extern std::filesystem::path folder;
extern bool found;

MenuState& Menu();
void RefreshMenu();      // reads helixsr.log (and helixsr.ini the first time) at most every two seconds
void SaveMenuSettings(); // writes Menu().settings to helixsr.ini
::Upscaler Backend();    // the configured upscaler of the game's API
void ChooseHelixSR();
void ChooseBackend(::Upscaler upscaler);
void OpenFolder();
} // namespace HelixSR::detail
