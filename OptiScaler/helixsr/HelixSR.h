#pragma once

// HelixSR Scaler: what this fork adds to OptiScaler. Everything HelixSR-specific lives in this folder, so OptiScaler's
// own files only change in a few marked places ("HelixSR Scaler:") and updates from OptiScaler merge easily.

#include <filesystem>
#include <string>

#include <imgui/imgui.h>

namespace HelixSR
{
// Finds the HelixSR folder next to this DLL (or the game's .exe) and points OptiScaler at it: the FidelityFX DLL
// paths and the upscaler, unless OptiScaler.ini sets them. Called once at startup, after OptiScaler's own paths.
void ApplyDefaults();

// The HelixSR folder was found
bool Found();
const std::filesystem::path& Folder();

// "1.0.0"
const char* Version();

// HelixSR is the upscaler running right now
bool IsActive();

// The simple menu is shown instead of OptiScaler's full menu
bool SimpleMenu();

enum class MenuAction
{
    None,
    Close,
};

// Draws the simple menu window. color() tone-maps colours for HDR games (OptiScaler's toneMapColor).
MenuAction RenderMenu(float scale, ImGuiWindowFlags flags, ImVec4 (*color)(const ImVec4&));

// A button at the top of OptiScaler's full menu that goes back to the simple menu
void RenderBackButton();
} // namespace HelixSR
