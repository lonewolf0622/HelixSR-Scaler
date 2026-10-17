#pragma once

// HelixSR Scaler: what this fork adds to OptiScaler. Everything HelixSR-specific lives in this folder, so OptiScaler's
// own files only change in a few marked places ("HelixSR Scaler:") and updates from OptiScaler merge easily.

#include <filesystem>
#include <string>

#include <imgui/imgui.h>

namespace HelixSR
{
// Finds the HelixSR folder and connects its SR provider to OptiScaler's FidelityFX backend. Alternative upscalers
// and frame-generation providers remain available. Called once at startup, after OptiScaler's own paths.
void ApplyDefaults();

// The HelixSR folder was found
bool Found();
// This build uses OptiScaler's FidelityFX input conversion instead of HelixSR's NGX adapter.
bool FsrInput();
bool IsProvider(const std::string& name);
int ProviderIndex();
const std::filesystem::path& Folder();

// helixsr.dll in the HelixSR folder (empty when the folder was not found)
std::filesystem::path DllPath();

// The HelixSR folder has frame generation (its helixsr-fg folder)
bool FrameGenFound();

// "1.0.0"
const char* Version();

// HelixSR is the upscaler running right now
bool IsActive();

// Selects HelixSR's FidelityFX provider (also through the Dx12 bridge for DirectX 11 games).
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
