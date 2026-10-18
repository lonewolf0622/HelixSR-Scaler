// The frame-generation route HelixSR Scaler picks by default (OptiScaler/helixsr/HelixSRFgRoute.h), per kind of game
// and with settings made in OptiScaler.ini.
#include "../../OptiScaler/helixsr/HelixSRFgRoute.h"

#include <cstdio>

using namespace HelixSR::FgRoute;

static int fails = 0;

static void check(const char* what, bool ok)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

static Settings On()
{
    Settings s;
    s.engine = true;
    s.switchedOn = true;
    s.streamline = true;
    return s;
}

static bool Is(const Route& r, std::optional<Input> i, std::optional<Output> o, std::optional<Provider> p,
               std::optional<bool> enabled = std::nullopt, std::optional<bool> hudfix = std::nullopt)
{
    return r.input == i && r.output == o && r.provider == p && r.enabled == enabled && r.hudfix == hudfix;
}

int main()
{
    Game dlssg, fsr, fsr30, xefg, none, fsrAndDlssg;
    dlssg.dlssg = true;
    fsr.fsrfg = true;
    fsr30.fsrfg30 = true;
    xefg.xefg = true;
    fsrAndDlssg.dlssg = fsrAndDlssg.fsrfg = true;

    // HelixSR on, everything auto
    check("DLSS-G game: nvngxfg + HelixSR (the game's Streamline presents)",
          Is(Decide(dlssg, On()), Input::NvngxFG, std::nullopt, Provider::HelixSR));
    check("DLSS-G game that also ships FSR FG: still nvngxfg",
          Is(Decide(fsrAndDlssg, On()), Input::NvngxFG, std::nullopt, Provider::HelixSR));
    check("FSR 3.1 FG game: fsrfg -> dlssg + HelixSR, FG on",
          Is(Decide(fsr, On()), Input::FSRFG, Output::DLSSG, Provider::HelixSR, true));
    check("FSR 3.0 FG game: fsrfg30 -> dlssg + HelixSR",
          Is(Decide(fsr30, On()), Input::FSRFG30, Output::DLSSG, Provider::HelixSR, true));
    check("XeSS FG game: xefg -> dlssg + HelixSR",
          Is(Decide(xefg, On()), Input::XeFG, Output::DLSSG, Provider::HelixSR, true));
    check("upscaler-only game: upscaler -> dlssg + HelixSR, HUD fix on",
          Is(Decide(none, On()), Input::Upscaler, Output::DLSSG, Provider::HelixSR, true, true));

    // HelixSR's frame generation off or not installed: nothing is set
    auto off = On();
    off.switchedOn = false;
    check("switched off: nothing set", Is(Decide(fsr, off), std::nullopt, std::nullopt, std::nullopt));
    auto missing = On();
    missing.engine = false;
    check("not installed: nothing set", Is(Decide(dlssg, missing), std::nullopt, std::nullopt, std::nullopt));
    off.provider = Provider::HelixSR;
    check("switched off with FGNvngxReplacement = helixsr in the ini: provider none",
          Is(Decide(dlssg, off), std::nullopt, std::nullopt, Provider::None));

    // OptiScaler.ini's choices stay
    auto amd = On();
    amd.output = Output::FSRFG;
    check("FGOutput = fsrfg (AMD FSR FG) in the ini: HelixSR not used",
          Is(Decide(fsr, amd), std::nullopt, std::nullopt, std::nullopt));
    auto intel = On();
    intel.output = Output::XeFG;
    check("FGOutput = xefg (Intel XeSS FG) in the ini: HelixSR not used",
          Is(Decide(none, intel), std::nullopt, std::nullopt, std::nullopt));
    auto nofg = On();
    nofg.output = Output::NoFG;
    check("FGOutput = nofg in the ini: nothing set", Is(Decide(none, nofg), std::nullopt, std::nullopt, std::nullopt));
    auto nvngxNoOut = On();
    nvngxNoOut.input = Input::NvngxFG;
    nvngxNoOut.output = Output::NoFG;
    check("FGInput = nvngxfg with FGOutput = nofg (its usual pair): HelixSR provider",
          Is(Decide(dlssg, nvngxNoOut), std::nullopt, std::nullopt, Provider::HelixSR));
    auto nukems = On();
    nukems.provider = Provider::Nukems;
    check("FGNvngxReplacement = nukems in the ini stays",
          Is(Decide(dlssg, nukems), Input::NvngxFG, std::nullopt, std::nullopt));
    auto upInput = On();
    upInput.input = Input::Upscaler;
    check("FGInput = upscaler in the ini on a DLSS-G game: upscaler -> dlssg",
          Is(Decide(dlssg, upInput), std::nullopt, Output::DLSSG, Provider::HelixSR, true, true));
    auto fgOff = On();
    fgOff.enabled = false;
    fgOff.hudfix = false;
    check("[FrameGen] Enabled and HUDFix from the ini stay",
          Is(Decide(none, fgOff), Input::Upscaler, Output::DLSSG, Provider::HelixSR));

    // FGPresent = optiscaler written in the ini: the game's DLSS-G inputs, FSR FG swapchain
    auto present = On();
    present.optiPresent = true;
    check("FGPresent = optiscaler: dlssg -> fsrfg + HelixSR",
          Is(Decide(dlssg, present), Input::DLSSG, Output::FSRFG, Provider::HelixSR));

    // no streamline folder: no dlssg output
    auto nosl = On();
    nosl.streamline = false;
    check("no streamline folder: nothing set for non-DLSS-G games",
          Is(Decide(fsr, nosl), std::nullopt, std::nullopt, std::nullopt));
    check("no streamline folder: DLSS-G games still nvngxfg",
          Is(Decide(dlssg, nosl), Input::NvngxFG, std::nullopt, Provider::HelixSR));

    std::printf("\n%s\n", fails == 0 ? "all passed" : "FAILED");
    return fails == 0 ? 0 : 1;
}
