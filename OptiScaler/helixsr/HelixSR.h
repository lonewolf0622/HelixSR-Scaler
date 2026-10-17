#pragma once

// HelixSR Scaler: what this fork adds to OptiScaler. Everything HelixSR-specific lives in this folder, so OptiScaler's
// own files only change in a few marked places ("HelixSR Scaler:") and updates from OptiScaler merge easily.

#include <filesystem>
#include <string>

#include <imgui/imgui.h>

namespace HelixSR
{
// Finds the HelixSR folder next to this DLL (or the game's .exe) and points OptiScaler at it: HelixSR as the DLSS
// upscaler and the DLSS frame-generation provider, unless OptiScaler.ini sets them. Called once at startup, after
// OptiScaler's own paths.
void ApplyDefaults();

// The HelixSR folder was found
bool Found();
const std::filesystem::path& Folder();

// helixsr.dll in the HelixSR folder (empty when the folder was not found)
std::filesystem::path DllPath();

// The HelixSR folder has frame generation (its helixsr-fg folder)
bool FrameGenFound();

// "1.0.0"
const char* Version();

// Menu name of a FidelityFX upscaler or frame-generation version: "FSR <name>", except HelixSR's own entry, which is
// shown as plain "HelixSR" (its version name, e.g. "HelixSR (3.1.5)", keeps the version OptiScaler reads from it)
inline std::string FfxVersionLabel(const char* name)
{
    const std::string n = name ? name : "";
    return n.rfind("HelixSR", 0) == 0 ? std::string("HelixSR") : "FSR " + n;
}

// HelixSR is the upscaler running right now
bool IsActive();

// Selects HelixSR (OptiScaler's DLSS upscaler with HelixSR's DLL; DLSS w/Dx12 in DirectX 11 games) as the upscaler
void Choose();

// Frame generation quality Best / Fast / Fastest ([HelixSR] QuickFlow = auto | on | off | fastest, auto = off;
// helixsr.ini [FrameGeneration] QuickFlow), drawn in the menu's frame generation section. Applies at the next game
// start.
void RenderQuickFlow();

// HelixSR's settings, drawn under the upscaler list while HelixSR is the upscaler:
// speed boost (int8), GPU mode and the folder button. Writes helixsr.ini; the game has to restart for them to apply.
void RenderSettings(float scale, ImVec4 (*color)(const ImVec4&));

// The HUD (HelixSRHud.cpp): the top of the menu window, with the upscaler choice, HelixSR's settings, live timings and
// HelixSR's state, in the style of a game overlay. OptiScaler's settings follow under it.
bool HudEnabled();
void RenderHud(float scale, ImVec4 (*color)(const ImVec4&));

// The HUD's shapes for OptiScaler's own widgets (square corners, accent lines); called after OptiScaler's theme
void ApplyHudStyle();
} // namespace HelixSR
