#pragma once

// HelixSR Scaler: HelixSR frame generation's quality, as OptiScaler.ini [HelixSR] keeps it and as HelixSR's DLL reads it
// from helixsr.ini [DlssFrameGeneration]. No Windows or OptiScaler headers here, so the rules are tested on their own
// (helixsr/tests/test_fg_quality.cpp).
//
//   OptiScaler.ini [HelixSR]                       helixsr.ini [DlssFrameGeneration]
//   FGPreset  = auto | c | full   (auto = c)        Preset    = c | full
//   QuickFlow = auto | off | fast | fastest         QuickFlow = off | on | fastest
//               (auto = off)                                    (always off with Preset = c)
//
// Light (preset C, NVIDIA's light mode) is the default. QuickFlow (fast / fastest) only runs with Full: HelixSR's DLL
// has no QuickFlow program for preset C. Both apply at the next game start.

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace HelixSR::FgQuality
{
enum class Preset : int
{
    Light = 0, // NVIDIA's preset C (light mode), the default
    Full = 1,
};

enum class QuickFlow : int
{
    Off = 0,
    Fast = 1,    // QuickFlow
    Fastest = 2, // QuickFlow at half size
};

inline std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);
    return s;
}

// OptiScaler.ini's FGPreset; nullopt for auto (and anything unknown)
inline std::optional<Preset> ParsePreset(const std::string& value)
{
    const auto v = Lower(value);
    if (v == "c" || v == "light")
        return Preset::Light;
    if (v == "full" || v == "default")
        return Preset::Full;
    return std::nullopt;
}

// OptiScaler.ini's QuickFlow (on / true / 1 from earlier versions mean fast); nullopt for auto (and anything unknown)
inline std::optional<QuickFlow> ParseQuickFlow(const std::string& value)
{
    const auto v = Lower(value);
    if (v == "off" || v == "false" || v == "0")
        return QuickFlow::Off;
    if (v == "fast" || v == "on" || v == "true" || v == "1" || v == "quickflow")
        return QuickFlow::Fast;
    if (v == "fastest")
        return QuickFlow::Fastest;
    return std::nullopt;
}

inline const char* OptiScalerPreset(std::optional<Preset> p)
{
    return !p ? "auto" : *p == Preset::Full ? "full" : "c";
}

inline const char* OptiScalerQuickFlow(std::optional<QuickFlow> q)
{
    return !q ? "auto" : *q == QuickFlow::Fastest ? "fastest" : *q == QuickFlow::Fast ? "fast" : "off";
}

inline Preset EffectivePreset(std::optional<Preset> p) { return p.value_or(Preset::Light); }

// QuickFlow as it runs: off unless Full
inline QuickFlow EffectiveQuickFlow(std::optional<Preset> p, std::optional<QuickFlow> q)
{
    return EffectivePreset(p) == Preset::Full ? q.value_or(QuickFlow::Off) : QuickFlow::Off;
}

inline const char* HelixPreset(std::optional<Preset> p) { return EffectivePreset(p) == Preset::Full ? "full" : "c"; }

inline const char* HelixQuickFlow(std::optional<Preset> p, std::optional<QuickFlow> q)
{
    const auto e = EffectiveQuickFlow(p, q);
    return e == QuickFlow::Fastest ? "fastest" : e == QuickFlow::Fast ? "on" : "off";
}
} // namespace HelixSR::FgQuality
