#include "pch.h"

// HelixSR Scaler: HelixSR's FidelityFX DLL (HelixSR-fsrfg\amd_fidelityfx_dx12.dll) as OptiScaler's FFX upscaler
// module, for games where helixsr.dll (the DLSS route) is not installed. FidelityFX is DirectX 12 only, so DirectX 11
// and Vulkan games run it through OptiScaler's Dx12 interop (FSR w/Dx12, ffx_12). HelixSR's DLL lists its own entry
// first and AMD's FSR versions after it (forwarded to amd_fidelityfx_upscaler_dx12.amd.dll next to it), so FSR 3.1
// and FSR 4 stay selectable in the FFX upscaler list.

#include "HelixSR.h"
#include "HelixSRInternal.h"

#include "Config.h"
#include "State.h"
#include "Util.h"

namespace fs = std::filesystem;

namespace
{
const wchar_t* kFsrFolder = L"HelixSR-fsrfg";
const wchar_t* kFsrDll = L"amd_fidelityfx_dx12.dll";
// AMD's FSR upscaler next to HelixSR's DLL under the name HelixSR's DLL looks for (it forwards AMD's versions to it)
const wchar_t* kAmdUpscalerForHelixSR = L"amd_fidelityfx_upscaler_dx12.amd.dll";
// AMD's FSR upscaler OptiScaler ships in its folder
const wchar_t* kAmdUpscaler = L"amd_fidelityfx_upscaler_dx12.dll";

fs::path fsrDll;
bool fsrDllFound = false;

bool SameFile(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    return fs::equivalent(a, b, ec) && !ec;
}

// AMD's FSR for HelixSR's list: OptiScaler's amd_fidelityfx_upscaler_dx12.amd.dll (else .dll) linked (else copied) next to HelixSR's
// DLL as amd_fidelityfx_upscaler_dx12.amd.dll, unless that file or a my-fsr folder (the user's own FSR DLLs) is there
void ProvideAmdUpscaler(const fs::path& folder)
{
    std::error_code ec;
    const auto target = folder / kAmdUpscalerForHelixSR;

    if (fs::is_directory(folder / L"my-fsr", ec))
    {
        LOG_INFO("HelixSR Scaler: AMD's FSR for HelixSR's list comes from its my-fsr folder");
        return;
    }

    // the package's amd_fidelityfx_upscaler_dx12.dll is HLX FSR 4, with AMD's own next to it as .amd.dll
    const auto main = fs::path(Config::Instance()->MainDllPath.value_or(L""));
    auto source = main / kAmdUpscalerForHelixSR;
    if (!fs::is_regular_file(source, ec))
        source = main / kAmdUpscaler;
    const bool haveSource = fs::is_regular_file(source, ec);

    if (fs::exists(target, ec))
    {
        // a newer OptiScaler DLL (an update of HelixSR Scaler) replaces an older copy; a hard link is the file itself
        if (!haveSource || SameFile(source, target) ||
            fs::last_write_time(target, ec) >= fs::last_write_time(source, ec))
            return;

        fs::remove(target, ec);
    }

    if (!haveSource)
    {
        LOG_WARN("HelixSR Scaler: no {} in {}: HelixSR's FidelityFX list has HelixSR only (no AMD FSR 3 / FSR 4)",
                 wstring_to_string(kAmdUpscaler), wstring_to_string(source.parent_path().wstring()));
        return;
    }

    const char* how = "linked";
    if (!CreateHardLinkW(target.c_str(), source.c_str(), nullptr))
    {
        how = "copied";
        if (!CopyFileW(source.c_str(), target.c_str(), TRUE))
        {
            LOG_WARN("HelixSR Scaler: could not put AMD's FSR next to HelixSR's DLL ({}): error {}",
                     wstring_to_string(target.wstring()), GetLastError());
            return;
        }
    }

    LOG_INFO("HelixSR Scaler: AMD's FSR {} as {} for HelixSR's FidelityFX list (FSR 3 / FSR 4 next to HelixSR)", how,
             wstring_to_string(target.wstring()));
}
} // namespace

namespace HelixSR
{
bool FsrDllFound() { return fsrDllFound; }

const fs::path& FsrDllPath() { return fsrDll; }

const char* ActiveFfxName()
{
    auto& state = State::Instance();
    auto feature = state.currentFeature;
    if (feature == nullptr || !feature->IsInited())
        return nullptr;

    const auto type = feature->GetUpscalerType();
    if (!(type == ::Upscaler::FFX_on12 || (type == ::Upscaler::FFX && state.api == DX12)))
        return nullptr;

    const auto& names = state.ffxUpscalerVersionsDx12.names;
    const int index = Config::Instance()->FfxUpscalerIndex.value_or_default();
    return index >= 0 && index < (int) names.size() ? names[index] : nullptr;
}

std::string ActiveUpscalerName()
{
    if (IsActive())
        return Names::kHelixUpscalerName;
    if (const char* name = ActiveFfxName(); Names::IsHlx(name))
        return Names::Label(name);
    return {};
}

bool IsActiveFfx()
{
    if (!fsrDllFound)
        return false;

    auto& state = State::Instance();
    auto feature = state.currentFeature;
    if (feature == nullptr || !feature->IsInited())
        return false;

    // HelixSR's DLL is the DirectX 12 FFX module: FSR in DirectX 12 games, FSR w/Dx12 in DirectX 11 and Vulkan games
    const auto type = feature->GetUpscalerType();
    if (!(type == ::Upscaler::FFX_on12 || (type == ::Upscaler::FFX && state.api == DX12)))
        return false;

    const auto& names = state.ffxUpscalerVersionsDx12.names;
    const int index = Config::Instance()->FfxUpscalerIndex.value_or_default();
    return index >= 0 && index < (int) names.size() && Names::IsHelixSR(names[index]);
}
} // namespace HelixSR

namespace HelixSR::detail
{
void ApplyFsrDllDefaults()
{
    auto cfg = Config::Instance();
    std::error_code ec;

    const fs::path places[] = { Util::ExePath().parent_path() / kFsrFolder / kFsrDll,
                                Util::DllPath().parent_path() / kFsrFolder / kFsrDll };

    for (const auto& p : places)
    {
        if (fs::is_regular_file(p, ec))
        {
            fsrDll = fs::absolute(p, ec);
            fsrDllFound = true;
            break;
        }
    }

    if (!fsrDllFound)
        return;

    // A FidelityFX upscaler DLL chosen in OptiScaler.ini stays: HelixSR is then only used when that is HelixSR's
    if (cfg->FfxDx12SRPath.has_value())
    {
        auto chosen = fs::path(cfg->FfxDx12SRPath.value());
        if (fs::is_directory(chosen, ec))
            chosen /= kAmdUpscaler; // OptiScaler loads the upscaler DLL by that name from a folder

        if (!SameFile(chosen, fsrDll))
        {
            LOG_INFO("HelixSR Scaler: HelixSR's FidelityFX DLL found in {}, but FfxDx12SRPath picks {}; not used",
                     wstring_to_string(fsrDll.parent_path().wstring()), wstring_to_string(chosen.wstring()));
            fsrDllFound = false;
            return;
        }
    }
    else
    {
        cfg->FfxDx12SRPath.set_volatile_value(fsrDll.wstring());
    }

    LOG_INFO("HelixSR Scaler: HelixSR's FidelityFX DLL {} is the FFX upscaler module",
             wstring_to_string(fsrDll.wstring()));

    ProvideAmdUpscaler(fsrDll.parent_path());

    // HelixSR is the first entry of its DLL's list (FfxUpscalerIndex 0), with AMD's FSR after it. DirectX 12 games run
    // it as FSR (ffx); FidelityFX is DirectX 12 only, so DirectX 11 and Vulkan games run it as FSR w/Dx12 (ffx_12).
    // An upscaler set in OptiScaler.ini (or in the menu) stays.
    if (!cfg->Dx12Upscaler.has_value())
        cfg->Dx12Upscaler.set_volatile_value(::Upscaler::FFX);

    if (!cfg->Dx11Upscaler.has_value())
        cfg->Dx11Upscaler.set_volatile_value(::Upscaler::FFX_on12);

    if (!cfg->VulkanUpscaler.has_value())
        cfg->VulkanUpscaler.set_volatile_value(::Upscaler::FFX_on12);

    LOG_INFO("HelixSR Scaler: upscalers Dx12={}, Dx11={}, Vulkan={} (FFX upscaler index {})",
             UpscalerToCode(cfg->Dx12Upscaler.value_or_default()), UpscalerToCode(cfg->Dx11Upscaler.value_or_default()),
             UpscalerToCode(cfg->VulkanUpscaler.value_or_default()), cfg->FfxUpscalerIndex.value_or_default());
}
} // namespace HelixSR::detail
