// Copyright 2026 lonewolf0622. GPL-3.0-or-later.
// CPU-only check of marker detection; no Scaler DLL or graphics library is loaded.
#include "StartupRecovery.h"
#include <cstdio>

int main()
{
    unsigned checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::printf("FAIL: %s\n", #expr); return 1; } } while (0)
    const std::filesystem::path folder = std::filesystem::current_path();
    CHECK(!HelixSR::detail::IncompleteStart(folder));
    const auto boot = folder / L"helixsr-boot.starting";
    HANDLE active = CreateFileW(boot.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(active != INVALID_HANDLE_VALUE);
    CHECK(!HelixSR::detail::IncompleteStart(folder));
    const auto feature = folder / L"helixsr-fg.starting";
    HANDLE stale = CreateFileW(feature.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(stale != INVALID_HANDLE_VALUE); CloseHandle(stale);
    CHECK(!HelixSR::detail::IncompleteStart(folder));
    CloseHandle(active);
    CHECK(HelixSR::detail::IncompleteStart(folder));
    CHECK(DeleteFileW(boot.c_str()));
    CHECK(HelixSR::detail::IncompleteStart(folder));
    CHECK(DeleteFileW(feature.c_str()));
    CHECK(!HelixSR::detail::IncompleteStart(folder));
    std::printf("%u recovery checks passed\n", checks);
    return 0;
}
