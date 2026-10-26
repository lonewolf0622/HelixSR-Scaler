#pragma once

// HelixSR Scaler: what this fork adds to OptiScaler. Everything HelixSR-specific lives in this folder, so OptiScaler's
// own files only change in a few marked places ("HelixSR Scaler:") and updates from OptiScaler merge easily.

#include <filesystem>
#include <string>

#include <imgui/imgui.h>

#include "HelixSRNames.h"

namespace HelixSR
{
// Finds HelixSR next to this DLL (or the game's .exe) and points OptiScaler's upscaler at it (HelixSR's FidelityFX DLL
// as the FFX upscaler module, or an older install's helixsr.dll as DLSS), unless OptiScaler.ini sets them. Frame
// generation is left to OptiScaler.ini ([FrameGen] FGNvngxReplacement = HelixSR). Called once at startup, after
// OptiScaler's own paths.
void ApplyDefaults();

// The HelixSR folder was found
bool Found();
const std::filesystem::path& Folder();

// helixsr.dll in the HelixSR folder (empty when the folder was not found)
std::filesystem::path DllPath();

// HelixSR has frame generation (its helixsr-fg folder next to HelixSR's DLL)
bool FrameGenFound();

// Without helixsr.dll in a HelixSR folder: HelixSR's FidelityFX DLL (HelixSR-fsrfg\amd_fidelityfx_dx12.dll, or
// [Libraries] FfxDx12SRPath) as the DLSS frame-generation provider, when it serves DLSS frame generation too (it
// exports HelixSR_NgxFgVersion). Loaded, or null. Files in its folder then load as they are (LoadsAsIs).
HMODULE LoadFfxFrameGenDll();

// A file in the HelixSR folder, or in the folder of the FidelityFX DLL that serves DLSS frame generation: it loads as
// it is. HelixSR's frame generation loads NVIDIA's NGX core (helixsr-fg\_nvngx.dll) and its frame-generation module by
// full path; answering those with this DLL makes HelixSR call itself.
bool LoadsAsIs(const std::filesystem::path& path);

// "1.0.0"
const char* Version();

// Menu name of a FidelityFX upscaler or frame-generation version: "FSR <name>", except our own: HelixSR's entry is
// "HLX DLSS", an HLX entry ("HLX FSR 4 (4.1.2)") its name without the version OptiScaler reads from it
inline std::string FfxVersionLabel(const char* name) { return Names::Label(name); }

// HelixSR's FidelityFX DLL (HelixSR-fsrfg\amd_fidelityfx_dx12.dll next to the game's .exe or this DLL, or the
// FfxDx12SRPath set in OptiScaler.ini when that is HelixSR's) was found. Without helixsr.dll it is OptiScaler's FFX
// upscaler module: FSR w/Dx12 is HelixSR in DirectX 11 and Vulkan games, unless OptiScaler.ini picks an upscaler.
bool FsrDllFound();
const std::filesystem::path& FsrDllPath();

// HelixSR is installed in one of its forms (the HelixSR folder with helixsr.dll, or HelixSR's FidelityFX DLL)
inline bool Installed() { return Found() || FsrDllFound(); }

// The upscaler running now is HelixSR's FidelityFX entry (FSR with HelixSR's DLL as the FFX upscaler module)
bool IsActiveFfx();

// HelixSR is the upscaler running right now (as OptiScaler's DLSS upscaler or as its FidelityFX entry)
bool IsActive();

// The FidelityFX version name of the FFX upscaler running now (nullptr when it is not an FFX upscaler)
const char* ActiveFfxName();

// The menu name of our upscaler running right now: "HLX DLSS" (HelixSR) or an HLX FidelityFX entry ("HLX FSR 4");
// empty when another upscaler runs
std::string ActiveUpscalerName();

// Selects HelixSR (OptiScaler's DLSS upscaler with HelixSR's DLL; DLSS w/Dx12 in DirectX 11 games) as the upscaler
void Choose();

// Frame generation quality: NVIDIA's full frame generation with QuickFlow Best (off) / Fast / Fastest (preset C removed)
// ([HelixSR] FGPreset and QuickFlow; helixsr.ini [DlssFrameGeneration] Preset and QuickFlow, see HelixSRFgQuality.h),
// drawn in the menu's frame generation section. Applies at the next game start.
void RenderFrameGenQuality(float scale);

// The menu's frame-generation choice (HelixSR / AMD FSR / Intel XeSS / Off), above OptiScaler's own FG lists: writes
// OptiScaler.ini's [FrameGen] lines as a user would, applies at the next game start
void RenderFrameGenChoice(float scale);

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
