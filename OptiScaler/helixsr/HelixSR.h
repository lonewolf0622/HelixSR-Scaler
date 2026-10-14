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

// The HelixSR section of OptiScaler's menu: status, the few HelixSR settings, the folder button. Drawn inside the
// menu's own window. color() tone-maps colours for HDR (OptiScaler's toneMapColor).
void RenderSection(float scale, ImVec4 (*color)(const ImVec4&));
} // namespace HelixSR
