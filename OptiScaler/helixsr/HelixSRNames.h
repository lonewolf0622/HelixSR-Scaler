#pragma once

// HelixSR Scaler: how the FidelityFX upscaler list is shown when HelixSR's FidelityFX DLL is the FFX upscaler module.
// HelixSR's DLL lists its own entry first ("HelixSR", or "HelixSR (3.1.5)" with its ListName = full), then the AMD
// FSR versions it forwards to (FSR 4.x, FSR 3.1.x, ...). No Windows or OptiScaler headers here, so the rules are
// tested on their own (helixsr/tests/test_names.sh).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

namespace HelixSR::Names
{
// HelixSR's own entry in a FidelityFX version list
inline bool IsHelixSR(const char* name) { return name != nullptr && std::strncmp(name, "HelixSR", 7) == 0; }

// Menu name of a FidelityFX upscaler or frame-generation version: "FSR <name>", HelixSR's own entry plain "HelixSR"
inline std::string Label(const char* name)
{
    const std::string n = name ? name : "";
    return IsHelixSR(name) ? std::string("HelixSR") : "FSR " + n;
}

struct Version
{
    unsigned major = 0;
    unsigned minor = 0;
    unsigned patch = 0;

    bool operator<(const Version& o) const
    {
        if (major != o.major)
            return major < o.major;
        if (minor != o.minor)
            return minor < o.minor;
        return patch < o.patch;
    }
};

// The first "a.b.c" in a version name (as OptiScaler's feature_version reads it); HelixSR's entry without one is
// 3.1.5, the FidelityFX upscaler API level HelixSR implements (OptiScaler's version checks need a number)
inline Version Parse(const char* name)
{
    Version v;
    for (const char* p = name ? name : ""; *p; ++p)
    {
        if (*p >= '0' && *p <= '9' && std::sscanf(p, "%u.%u.%u", &v.major, &v.minor, &v.patch) == 3)
            return v;
    }

    v = {};
    if (IsHelixSR(name))
        v = { 3, 1, 5 };
    return v;
}

// Indices into a version list: HelixSR's entry first (the default, index 0 in HelixSR's own list), then newest first.
// Without a HelixSR entry this is OptiScaler's order (newest first, ties in list order).
inline std::vector<int> Order(const std::vector<const char*>& names)
{
    std::vector<int> order(names.size());
    std::iota(order.begin(), order.end(), 0);

    std::stable_sort(order.begin(), order.end(),
                     [&names](int a, int b)
                     {
                         const bool ha = IsHelixSR(names[a]), hb = IsHelixSR(names[b]);
                         if (ha != hb)
                             return ha;
                         return Parse(names[b]) < Parse(names[a]);
                     });

    return order;
}

// The entries the upscaler list shows for one FidelityFX backend: the newest AMD FSR, and HelixSR's entry next to it
// when the list has one (so picking "FSR" never switches HelixSR off, and picking HelixSR never needs the FFX list)
inline std::vector<int> ListEntries(const std::vector<const char*>& names)
{
    std::vector<int> entries;
    int helix = -1, newest = -1;

    for (int n : Order(names))
    {
        if (IsHelixSR(names[n]))
        {
            if (helix < 0)
                helix = n;
        }
        else if (newest < 0)
        {
            newest = n;
        }
    }

    if (helix >= 0)
        entries.push_back(helix);
    if (newest >= 0)
        entries.push_back(newest);

    return entries;
}
} // namespace HelixSR::Names
