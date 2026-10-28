// How the upscaler lists name and order HelixSR's FidelityFX entry next to AMD's FSR
// (OptiScaler/helixsr/HelixSRNames.h). The version lists are the ones HelixSR's DLL reports: its own entry first, then
// the versions of AMD's FSR DLL.
#include "../../OptiScaler/helixsr/HelixSRNames.h"

#include <cstdio>

using namespace HelixSR::Names;

static int fails = 0;

static void check(const char* what, bool ok)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

static bool same(const std::vector<int>& a, const std::vector<int>& b) { return a == b; }

int main()
{
    // names shown
    check("HelixSR entry is named HLX DLSS", Label("HelixSR") == "HLX DLSS");
    check("HelixSR (3.1.5) entry is named HLX DLSS", Label("HelixSR (3.1.5)") == "HLX DLSS");
    check("HLX FSR 4 (4.1.2) is named HLX FSR 4", Label("HLX FSR 4 (4.1.2)") == "HLX FSR 4");
    check("an HLX name without a version stays as it is", Label("HLX FSR 4") == "HLX FSR 4");
    check("HLX FSR 4 (4.1.2) keeps version 4.1.2 for OptiScaler", Parse("HLX FSR 4 (4.1.2)").major == 4 &&
          Parse("HLX FSR 4 (4.1.2)").minor == 1 && Parse("HLX FSR 4 (4.1.2)").patch == 2);
    check("HLX and HelixSR are our own upscalers, AMD's are not", IsOwnUpscaler("HLX FSR 4 (4.1.2)") &&
          IsOwnUpscaler("HelixSR") && !IsOwnUpscaler("4.1.2") && !IsOwnUpscaler(nullptr));
    check("AMD entries keep the FSR prefix", Label("4.1.1") == "FSR 4.1.1" && Label("3.1.5") == "FSR 3.1.5");
    check("no name", Label(nullptr) == "FSR ");

    // version used by OptiScaler's checks
    const auto h = Parse("HelixSR"), hv = Parse("HelixSR (3.1.5)"), f4 = Parse("4.1.1"), f3 = Parse("FSR 3.1.4");
    check("HelixSR without a version is 3.1.5", h.major == 3 && h.minor == 1 && h.patch == 5);
    check("HelixSR (3.1.5) is 3.1.5", hv.major == 3 && hv.minor == 1 && hv.patch == 5);
    check("4.1.1 parses", f4.major == 4 && f4.minor == 1 && f4.patch == 1);
    check("FSR 3.1.4 parses", f3.major == 3 && f3.minor == 1 && f3.patch == 4);
    check("other names without a version are 0.0.0", Parse("abc").major == 0);

    // HelixSR's DLL with AMD's FSR 4.1.1 DLL on an FSR 4 GPU: HelixSR, 4.1.1, 3.1.5, 2.3.4
    const std::vector<const char*> rdna4 = { "HelixSR", "4.1.1", "3.1.5", "2.3.4" };
    check("order: HelixSR first, then newest", same(Order(rdna4), { 0, 1, 2, 3 }));
    check("upscaler list: HelixSR and FSR 4.1.1", same(ListEntries(rdna4), { 0, 1 }));

    // other GPUs: AMD's DLL has no FSR 4 there
    const std::vector<const char*> other = { "HelixSR", "3.1.5", "2.3.4" };
    check("upscaler list without FSR 4: HelixSR and FSR 3.1.5", same(ListEntries(other), { 0, 1 }));

    // HelixSR's entry is not first in the list (a DLL listing it later), ListName = full
    const std::vector<const char*> late = { "3.1.4", "HelixSR (3.1.5)", "4.0.2" };
    check("HelixSR first wherever it is listed", same(Order(late), { 1, 2, 0 }));
    check("upscaler list: HelixSR and the newest FSR", same(ListEntries(late), { 1, 2 }));

    // HelixSR's DLL alone (no AMD DLL next to it): one entry
    const std::vector<const char*> alone = { "HelixSR" };
    check("HelixSR alone: one entry", same(ListEntries(alone), { 0 }));

    // AMD's DLL without HelixSR: OptiScaler's own behaviour (newest first, one entry)
    const std::vector<const char*> amd = { "3.1.5", "4.1.1", "2.3.4" };
    check("without HelixSR: newest first", same(Order(amd), { 1, 0, 2 }));
    check("without HelixSR: one entry, the newest", same(ListEntries(amd), { 1 }));
    check("ties keep the list order", same(Order({ "3.1.5", "3.1.5" }), { 0, 1 }));
    check("empty list", ListEntries({}).empty());

    // HLX FSR 4: an upscaler of its own next to AMD's FSR (its own version id, AMD's FSR 4.1.2 listed after it)
    const std::vector<const char*> hlxRdna4 = { "HLX FSR 4 (4.1.2)", "4.1.2", "3.1.5", "2.3.4" };
    check("HLX FSR 4 first, then AMD's newest first", same(Order(hlxRdna4), { 0, 1, 2, 3 }));
    check("upscaler list: HLX FSR 4 and AMD's FSR 4.1.2", same(ListEntries(hlxRdna4), { 0, 1 }));
    const std::vector<const char*> hlxOther = { "HLX FSR 4 (4.1.2)", "3.1.5", "2.3.4" };
    check("upscaler list without AMD's FSR 4: HLX FSR 4 and FSR 3.1.5", same(ListEntries(hlxOther), { 0, 1 }));
    const std::vector<const char*> hlxTie = { "4.1.2", "HLX FSR 4 (4.1.2)" };
    check("HLX FSR 4 first over AMD's FSR of the same version", same(Order(hlxTie), { 1, 0 }));
    check("HLX FSR 4 alone: one entry", same(ListEntries({ "HLX FSR 4 (4.1.2)" }), { 0 }));

    std::printf("\n%s\n", fails == 0 ? "all passed" : "FAILED");
    return fails == 0 ? 0 : 1;
}
