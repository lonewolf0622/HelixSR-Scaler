#pragma once

// HelixSR Scaler: what HelixSR.cpp shares with the HUD (HelixSRHud.cpp). Not for OptiScaler's files.

#include <filesystem>
#include <string>

#include <imgui/imgui.h>

namespace HelixSR::detail
{
// helixsr.ini, as the menu shows it
struct Settings
{
    int sharpness = 0;     // Default, Off, Low, Medium, High
    int int8 = 0;          // Auto, On, Off
    int wave = 0;          // Auto, 64, 32
    std::string sharpener; // kept when it is one HelixSR knows
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

extern const char* const kSharpness[];
extern const char* const kInt8[];
extern const char* const kWave[];
inline constexpr int kSharpnessCount = 5;
inline constexpr int kInt8Count = 3;
inline constexpr int kWaveCount = 3;

inline constexpr const char* kSharpnessHelp =
    "How crisp the picture looks. Default is what this HelixSR version uses. If edges sparkle or shimmer, try Off.";
inline constexpr const char* kInt8Help = "Faster math (int8) on graphics cards that support it. Auto turns it on "
                                         "where it helps. If the picture shimmers or flickers, try Off.";
inline constexpr const char* kWaveHelp = "How many threads HelixSR runs together (wave size). Auto is right for most "
                                         "cards. For AMD RX 7000 / 9000 with a broken picture, try 64.";

extern std::filesystem::path folder;
extern bool found;

MenuState& Menu();
void RefreshMenu();      // reads helixsr.log (and helixsr.ini the first time) at most every two seconds
void SaveMenuSettings(); // writes Menu().settings to helixsr.ini
std::string BackendCode();
void ChooseHelixSR();
void ChooseBackend(const std::string& code); // another upscaler: "xess", "fsr21", "fsr22", "fsr31", "dlss"
void OpenFolder();
} // namespace HelixSR::detail
