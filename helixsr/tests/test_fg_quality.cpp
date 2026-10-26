// HelixSR frame generation's quality (OptiScaler/helixsr/HelixSRFgQuality.h): OptiScaler.ini's FGPreset / QuickFlow
// read and written back unchanged, and what goes to helixsr.ini [DlssFrameGeneration] (always Preset = full).
#include "../../OptiScaler/helixsr/HelixSRFgQuality.h"

#include <cstdio>
#include <cstring>

using namespace HelixSR::FgQuality;

static int fails = 0;

static void check(const char* what, bool ok)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

static bool Eq(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

int main()
{
    // OptiScaler.ini round trip: every value written reads back as the same choice and is written the same way
    for (const char* v : { "auto", "c", "full" })
    {
        const auto p = ParsePreset(v);
        check(v, Eq(OptiScalerPreset(p), v));
    }
    for (const char* v : { "auto", "off", "fast", "fastest" })
    {
        const auto q = ParseQuickFlow(v);
        check(v, Eq(OptiScalerQuickFlow(q), v));
    }

    // spellings people (and earlier versions) use
    check("FGPreset=Light is c", ParsePreset("Light") == Preset::Light);
    check("FGPreset= FULL  is full", ParsePreset(" FULL ") == Preset::Full);
    check("FGPreset=default (the DLL's older name) is full", ParsePreset("default") == Preset::Full);
    check("FGPreset=something is auto", !ParsePreset("something").has_value());
    check("QuickFlow=on (earlier versions) is fast", ParseQuickFlow("on") == QuickFlow::Fast);
    check("QuickFlow=on is written back as fast", Eq(OptiScalerQuickFlow(ParseQuickFlow("on")), "fast"));
    check("QuickFlow=true is fast", ParseQuickFlow("true") == QuickFlow::Fast);
    check("QuickFlow=0 is off", ParseQuickFlow("0") == QuickFlow::Off);
    check("QuickFlow=Fastest is fastest", ParseQuickFlow("Fastest") == QuickFlow::Fastest);

    // helixsr.ini: preset C is removed, so Preset = full always; QuickFlow off (best) by default
    check("default: Preset = full", Eq(HelixPreset(std::nullopt), "full"));
    check("older FGPreset=c: Preset = full", Eq(HelixPreset(Preset::Light), "full"));
    check("default: QuickFlow = off", Eq(HelixQuickFlow(std::nullopt, std::nullopt), "off"));
    check("Full: Preset = full", Eq(HelixPreset(Preset::Full), "full"));

    // QuickFlow with any (older) FGPreset; the DLL reads on / fastest
    check("Full + off: QuickFlow = off", Eq(HelixQuickFlow(Preset::Full, QuickFlow::Off), "off"));
    check("Full + fast: QuickFlow = on", Eq(HelixQuickFlow(Preset::Full, QuickFlow::Fast), "on"));
    check("Full + fastest: QuickFlow = fastest", Eq(HelixQuickFlow(Preset::Full, QuickFlow::Fastest), "fastest"));
    check("Full + auto: QuickFlow = off", Eq(HelixQuickFlow(Preset::Full, std::nullopt), "off"));
    for (auto q : { QuickFlow::Off, QuickFlow::Fast, QuickFlow::Fastest })
    {
        check("older FGPreset=c: QuickFlow kept", HelixQuickFlow(Preset::Light, q) == HelixQuickFlow(Preset::Full, q));
        check("auto: QuickFlow kept", EffectiveQuickFlow(std::nullopt, q) == q);
        check("older FGPreset=c: effective QuickFlow kept", EffectiveQuickFlow(Preset::Light, q) == q);
    }

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
