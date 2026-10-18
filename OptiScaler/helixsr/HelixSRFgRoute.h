#pragma once

// HelixSR Scaler: which frame-generation route HelixSR Scaler picks by default, from what the game has. No Windows or
// OptiScaler headers here, so the rules are tested on their own (helixsr/tests/test_fg_route.sh). The enums have
// OptiScaler's values (FGInput, FGOutput, FGNvngxReplacement in State.h; HelixSR.cpp checks that they match).
//
// HelixSR's frame generation is NVIDIA's DLSS Frame Generation run by HelixSR's DLL, as the nvngx_dlssg replacement
// (FGNvngxReplacement = helixsr). Who presents its frames ([HelixSR] FGPresent, auto by default):
//  - a game with DLSS Frame Generation: the game's own Streamline (FGInput = nvngxfg);
//  - any other DirectX 12 game: OptiScaler's FSR FG swapchain (FGOutput = fsrfg, the OptiFG route; HelixSR generates
//    in its frame-generation callback), with the inputs OptiScaler collects from the game's FSR 3.1 frame generation
//    (fsrfg), FSR 3.0 frame generation (fsrfg30), XeSS frame generation (xefg), or else from the upscaler OptiScaler
//    runs (upscaler, with the HUD fix for the game's UI).
// FGPresent = optiscaler also takes DLSS-G games to the FSR FG swapchain (FGInput = dlssg); FGPresent = streamline
// gives the other games OptiScaler's Streamline (FGOutput = dlssg) instead of the FSR FG swapchain.
// Settings OptiScaler.ini sets are never changed; only the ones it leaves on auto are filled in.

#include <cstdint>
#include <optional>
#include <string>

namespace HelixSR::FgRoute
{
enum class Input : uint32_t
{
    NoFG,
    Upscaler,
    DLSSG,
    NvngxFG,
    FSRFG,
    FSRFG30,
    XeFG,
};

enum class Output : uint32_t
{
    NoFG,
    FSRFG,
    DLSSG,
    XeFG,
    Reprojection,
};

enum class Provider : uint32_t
{
    None,
    Nukems,
    Arturs,
    FFX,
    Combo,
    HelixSR,
};

// What the game ships (found in its folder, not counting OptiScaler's or HelixSR's own files)
struct Game
{
    bool dlssg = false;   // Streamline's sl.dlss_g.dll
    bool fsrfg = false;   // FSR 3.1 frame generation (amd_fidelityfx_framegeneration_dx12.dll)
    bool fsrfg30 = false; // FSR 3.0 frame generation (ffx_frameinterpolation_x64.dll)
    bool xefg = false;    // libxess_fg.dll
};

// [HelixSR] FGPresent: who presents HelixSR's frames
enum class Present : uint32_t
{
    Auto,       // DLSS-G games: the game's Streamline (nvngxfg); the other games: OptiScaler's FSR FG swapchain
    OptiScaler, // OptiScaler's FSR FG swapchain for every game (DLSS-G games: FGInput = dlssg)
    Streamline, // DLSS-G games: the game's Streamline; the other games: OptiScaler's Streamline (FGOutput = dlssg)
};

struct Settings
{
    bool engine = false;     // HelixSR's frame generation is installed (FrameGenFound)
    bool switchedOn = true;  // [HelixSR] FrameGeneration = on (auto is off)
    bool streamline = false; // OptiScaler's streamline folder (sl.interposer.dll), needed by FGOutput = dlssg
    bool ffxFg = true;       // a FidelityFX frame-generation DLL for FGOutput = fsrfg (OptiScaler's or the game's)
    Present present = Present::Auto;

    // OptiScaler.ini's values, nullopt for auto
    std::optional<Input> input;
    std::optional<Output> output;
    std::optional<Provider> provider;
    std::optional<bool> enabled; // [FrameGen] Enabled
    std::optional<bool> hudfix;  // [OptiFG] HUDFix
};

// The values to set (volatile, never saved); nullopt: leave as it is
struct Route
{
    std::optional<Input> input;
    std::optional<Output> output;
    std::optional<Provider> provider;
    std::optional<bool> enabled;
    std::optional<bool> hudfix;
    std::string why; // for the log
};

inline const char* InputCode(Input i)
{
    switch (i)
    {
    case Input::Upscaler:
        return "upscaler";
    case Input::DLSSG:
        return "dlssg";
    case Input::NvngxFG:
        return "nvngxfg";
    case Input::FSRFG:
        return "fsrfg";
    case Input::FSRFG30:
        return "fsrfg30";
    case Input::XeFG:
        return "xefg";
    default:
        return "nofg";
    }
}

// The inputs the OptiFG route takes (OptiScaler's FSR FG swapchain presents, HelixSR generates)
inline bool OptiFgInput(Input i)
{
    return i == Input::DLSSG || i == Input::FSRFG || i == Input::FSRFG30 || i == Input::XeFG || i == Input::Upscaler;
}

inline Route Decide(const Game& game, const Settings& s)
{
    Route r;

    if (!s.engine || !s.switchedOn)
    {
        // HelixSR's frame generation off or not there: only a HelixSR provider written in OptiScaler.ini is taken
        // back (nothing would generate); every other choice stays OptiScaler's
        if (s.provider == Provider::HelixSR)
            r.provider = Provider::None;

        r.why = !s.switchedOn ? "HelixSR frame generation off ([HelixSR] FrameGeneration is not on)"
                              : "HelixSR frame generation not installed";
        return r;
    }

    // An output OptiScaler.ini picks other than DLSSG (AMD's FSR FG, XeFG, reprojection): that frame generation runs,
    // not HelixSR's (except the FGPresent = optiscaler route: game's DLSS-G inputs, FSR FG output, HelixSR generates)
    if (s.output == Output::NoFG && s.input != Input::NvngxFG)
    {
        r.why = "FGOutput = nofg from OptiScaler.ini: no frame generation";
        return r;
    }

    if (s.output.has_value() && s.output != Output::DLSSG && s.output != Output::NoFG)
    {
        const Input gameInput = s.input.value_or(game.dlssg ? Input::DLSSG : Input::NoFG);

        if (s.present == Present::OptiScaler && s.output == Output::FSRFG && gameInput == Input::DLSSG)
        {
            if (!s.input.has_value())
                r.input = Input::DLSSG;
            if (!s.provider.has_value())
                r.provider = Provider::HelixSR;
            r.why =
                "FGPresent = optiscaler: the game's DLSS-G inputs, OptiScaler's FSR FG swapchain, HelixSR generates";
            return r;
        }

        // FGOutput = fsrfg written with FGPresent = optiscaler (or FGNvngxReplacement = helixsr): the OptiFG route for
        // the input OptiScaler.ini names
        if (s.output == Output::FSRFG && s.input.has_value() && OptiFgInput(s.input.value()) &&
            (s.present == Present::OptiScaler || (s.present == Present::Auto && s.provider == Provider::HelixSR)))
        {
            if (!s.provider.has_value())
                r.provider = Provider::HelixSR;
            if (s.input != Input::DLSSG && !s.enabled.has_value())
                r.enabled = true;
            if (s.input == Input::Upscaler && !s.hudfix.has_value())
                r.hudfix = true;
            r.why = std::string("OptiFG route: FGInput = ") + InputCode(s.input.value()) +
                    ", OptiScaler's FSR FG swapchain presents, HelixSR generates";
            return r;
        }

        r.why = "OptiScaler.ini picks another frame generation output; HelixSR's frame generation is not used";
        return r;
    }

    Input input;
    if (s.input.has_value())
    {
        input = s.input.value();
        r.why = std::string("FGInput = ") + InputCode(input) + " from OptiScaler.ini";
    }
    else if (game.dlssg)
    {
        const bool opti = s.present == Present::OptiScaler;
        input = opti ? Input::DLSSG : Input::NvngxFG;
        r.why = opti ? "the game has DLSS Frame Generation: FGPresent = optiscaler (FGInput = dlssg)"
                     : "the game has DLSS Frame Generation: it runs HelixSR's (FGInput = nvngxfg)";
    }
    else if (game.fsrfg)
    {
        input = Input::FSRFG;
        r.why = "the game has FSR 3.1 frame generation: its inputs go to HelixSR (FGInput = fsrfg)";
    }
    else if (game.fsrfg30)
    {
        input = Input::FSRFG30;
        r.why = "the game has FSR 3.0 frame generation: its inputs go to HelixSR (FGInput = fsrfg30)";
    }
    else if (game.xefg)
    {
        input = Input::XeFG;
        r.why = "the game has XeSS frame generation: its inputs go to HelixSR (FGInput = xefg)";
    }
    else
    {
        input = Input::Upscaler;
        r.why = "no frame generation in the game: the upscaler's inputs go to HelixSR (FGInput = upscaler, HUD fix)";
    }

    if (input == Input::NoFG)
    {
        r.why = "FGInput = nofg from OptiScaler.ini";
        return r;
    }

    if (input == Input::NvngxFG)
    {
        // the game's own Streamline presents; OptiScaler's output stays off
        if (!s.input.has_value())
            r.input = input;
        if (!s.provider.has_value())
            r.provider = Provider::HelixSR;
        return r;
    }

    if (input == Input::DLSSG && s.present == Present::OptiScaler)
    {
        if (!s.input.has_value())
            r.input = input;
        if (!s.output.has_value())
            r.output = Output::FSRFG;
        if (!s.provider.has_value())
            r.provider = Provider::HelixSR;
        return r;
    }

    // the other inputs (FSR / XeSS frame generation, the upscaler): the OptiFG route, OptiScaler's FSR FG swapchain
    // presents and HelixSR generates in its callback. FGPresent = streamline keeps OptiScaler's Streamline (below),
    // and without a FidelityFX frame-generation DLL auto takes it too.
    if (input != Input::DLSSG && s.present != Present::Streamline && (s.ffxFg || s.present == Present::OptiScaler))
    {
        if (!s.input.has_value())
            r.input = input;
        if (!s.output.has_value())
            r.output = Output::FSRFG;
        if (!s.provider.has_value())
            r.provider = Provider::HelixSR;
        if (!s.enabled.has_value())
            r.enabled = true;
        if (input == Input::Upscaler && !s.hudfix.has_value())
            r.hudfix = true;
        r.why += ": OptiFG route, OptiScaler's FSR FG swapchain presents, HelixSR generates (FGOutput = fsrfg)";
        return r;
    }

    // OptiScaler's Streamline presents (FGOutput = dlssg), HelixSR as its nvngx_dlssg
    if (!s.streamline)
    {
        r = {};
        r.why = "no streamline folder next to OptiScaler.dll (sl.interposer.dll): FGOutput = dlssg can't run, "
                "HelixSR's frame generation is not set up for this game";
        return r;
    }

    r.why += s.present == Present::Streamline
                 ? ": FGPresent = streamline, OptiScaler's Streamline presents (FGOutput = dlssg)"
                 : ": no FidelityFX frame-generation DLL, OptiScaler's Streamline presents (FGOutput = dlssg)";

    if (!s.input.has_value())
        r.input = input;
    if (!s.output.has_value())
        r.output = Output::DLSSG;
    if (!s.provider.has_value())
        r.provider = Provider::HelixSR;

    // the game's DLSS-G switch drives FG with the DLSSG input; the others need FG switched on
    if (input != Input::DLSSG && !s.enabled.has_value())
        r.enabled = true;

    if (input == Input::Upscaler && !s.hudfix.has_value())
        r.hudfix = true;

    return r;
}
} // namespace HelixSR::FgRoute
