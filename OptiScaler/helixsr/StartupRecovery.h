// Copyright 2026 lonewolf0622. GPL-3.0-or-later.
#pragma once
#include <windows.h>
#include <filesystem>
#include <initializer_list>

namespace HelixSR::detail
{
inline bool IncompleteStart(const std::filesystem::path& folder)
{
    for (const auto* name : { L"helixsr-boot.starting", L"helixsr-fg.starting" })
    {
        const auto path = folder / name;
        // An active startup keeps its boot marker open without write sharing. Only an abandoned marker is recovery.
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
            return true;
        }
        if (GetLastError() == ERROR_SHARING_VIOLATION)
            return false; // its feature marker can belong to that same active instance
    }
    return false;
}
} // namespace HelixSR::detail
