#pragma once

// HelixSR Scaler: the FSR 4 hooks (models, presets, pipeline layouts) are found by byte patterns, so they go only into
// AMD's own DLLs. Another module loaded as the FidelityFX DLL (HelixSR's upscaler, HLX FSR 4) can contain a matching
// sequence, and patching it breaks that module.

#include <windows.h>
#include <cwchar>
#include <vector>

inline bool FSR4IsAmdModule(HMODULE module)
{
    wchar_t path[MAX_PATH];
    if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return false;

    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (size == 0)
        return false;

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data()))
        return false;

    struct LangCodePage
    {
        WORD language;
        WORD codePage;
    }* translations = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (LPVOID*) &translations, &bytes))
        return false;

    for (UINT i = 0; i < bytes / sizeof(LangCodePage); i++)
    {
        wchar_t key[64];
        swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\CompanyName", translations[i].language, translations[i].codePage);

        wchar_t* company = nullptr;
        UINT length = 0;
        if (VerQueryValueW(data.data(), key, (LPVOID*) &company, &length) && company != nullptr &&
            (wcsstr(company, L"Advanced Micro Devices") != nullptr || wcsstr(company, L"AMD") != nullptr))
            return true;
    }

    return false;
}
