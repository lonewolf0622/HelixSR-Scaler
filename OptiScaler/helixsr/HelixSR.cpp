#include "pch.h"

#include "HelixSR.h"
#include "HelixSRInternal.h"
#include "HelixSRFgRoute.h"
#include "HelixSRFgQuality.h"

#include "Config.h"
#include "State.h"
#include "Util.h"
#include "resource.h"
#include "proxies/NVNGX_Proxy.h"
#include "proxies/Ntdll_Proxy.h"

#include <shellapi.h>
#include <magic_enum.hpp>

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <format>
#include <mutex>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

// Shared with the HUD (HelixSRHud.cpp) through HelixSRInternal.h
namespace HelixSR::detail
{
const wchar_t* kFolder = L"HelixSR";
const wchar_t* kDll = L"helixsr.dll";     // HelixSR's DLSS DLL: OptiScaler's DLSS upscaler and frame generation
const wchar_t* kFGFolder = L"helixsr-fg"; // what frame generation needs (NVIDIA's core and frame generation)
// frame generation switched off ([HelixSR] FrameGeneration=off): the folder under this name, so neither HelixSR's DLL
// nor OptiScaler finds it (HelixSR's DLL has no switch of its own: it uses helixsr-fg when it is there)
const wchar_t* kFGFolderOff = L"helixsr-fg.off";
const wchar_t* kIni = L"helixsr.ini";
const wchar_t* kLog = L"helixsr.log";

fs::path folder;
bool found = false;

std::string Lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

std::string Trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// --- helixsr.ini ------------------------------------------------------------------------------------------------
// Line based, so the comments and examples in the file stay as they are.

struct IniFile
{
    std::vector<std::string> lines;
    std::string separator = " = "; // between a key and its value in lines this writes

    static IniFile Load(const fs::path& p)
    {
        IniFile f;
        std::ifstream in(p, std::ios::binary);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            f.lines.push_back(line);
        }
        return f;
    }

    bool Save(const fs::path& p) const
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        for (const auto& l : lines)
            out << l << "\r\n";
        return out.good();
    }

    static bool IsSection(const std::string& line, std::string* name)
    {
        auto t = Trim(line);
        if (t.size() < 2 || t.front() != '[' || t.back() != ']')
            return false;
        if (name != nullptr)
            *name = Lower(Trim(t.substr(1, t.size() - 2)));
        return true;
    }

    // Key of "key = value" (or of ";key = value" when commented is true), empty when the line is something else
    static std::string KeyOf(const std::string& line, bool commented)
    {
        auto t = Trim(line);
        if (commented)
        {
            if (t.empty() || t.front() != ';')
                return "";
            t = Trim(t.substr(1));
        }
        else if (t.empty() || t.front() == ';' || t.front() == '#')
        {
            return "";
        }

        const auto eq = t.find('=');
        if (eq == std::string::npos)
            return "";
        auto key = Trim(t.substr(0, eq));
        return key.find(' ') == std::string::npos ? Lower(key) : "";
    }

    // First line after [section] and the line after its last line; false when there is no [section]
    bool Range(const std::string& section, size_t& begin, size_t& end) const
    {
        bool found = false;
        begin = end = lines.size();
        std::string name;
        for (size_t i = 0; i < lines.size(); i++)
        {
            if (!IsSection(lines[i], &name))
                continue;
            if (found)
            {
                end = i;
                break;
            }
            if (name == Lower(section))
            {
                found = true;
                begin = i + 1;
            }
        }
        return found;
    }

    std::optional<std::string> Get(const std::string& section, const std::string& key) const
    {
        size_t b, e;
        Range(section, b, e);
        for (size_t i = b; i < e; i++)
        {
            if (KeyOf(lines[i], false) != Lower(key))
                continue;
            auto v = lines[i].substr(lines[i].find('=') + 1);
            if (auto c = v.find(';'); c != std::string::npos)
                v = v.substr(0, c);
            return Trim(v);
        }
        return std::nullopt;
    }

    // value nullopt: the line is removed, HelixSR uses its default
    void Set(const std::string& section, const std::string& key, const std::optional<std::string>& value)
    {
        size_t b, e;
        if (!Range(section, b, e))
        {
            if (!value.has_value())
                return;
            if (!lines.empty() && !Trim(lines.back()).empty())
                lines.push_back("");
            lines.push_back("[" + section + "]");
            lines.push_back(key + separator + value.value());
            return;
        }

        for (size_t i = b; i < e; i++)
        {
            if (KeyOf(lines[i], false) != Lower(key))
                continue;

            if (value.has_value())
            {
                lines[i] = key + separator + value.value();
                // more than one line for the key: the first one wins, the rest go
                for (size_t j = e; j-- > i + 1;)
                    if (KeyOf(lines[j], false) == Lower(key))
                        lines.erase(lines.begin() + j);
            }
            else
            {
                for (size_t j = e; j-- > i;)
                    if (KeyOf(lines[j], false) == Lower(key))
                        lines.erase(lines.begin() + j);
            }
            return;
        }

        if (!value.has_value())
            return;

        // after the commented-out example of the key, else at the end of the section
        size_t at = e;
        for (size_t i = b; i < e; i++)
        {
            if (KeyOf(lines[i], true) == Lower(key))
            {
                at = i + 1;
                break;
            }
        }
        if (at == e)
        {
            while (at > b && Trim(lines[at - 1]).empty())
                at--;
        }
        lines.insert(lines.begin() + at, key + separator + value.value());
    }
};

// --- Settings shown in the menu ------------------------------------------------------------------------------------

// Sharpening is OptiScaler's own (RCAS, its menu and [CAS] / [Sharpness]); HelixSR's sharpener stays off
extern const char* const kInt8[] = { "Auto", "On", "Off" };
extern const char* const kWave[] = { "Auto", "64", "32" };
extern const char* const kFrameGen[] = { "On", "Off" };

// --- Settings: OptiScaler.ini, with HelixSR's part copied to helixsr.ini ------------------------------------------
// HelixSR reads only helixsr.ini next to its DLL. OptiScaler.ini is the one file people edit: [HelixSR] for HelixSR's
// settings, [Sharpness] and [CAS] for sharpening. HelixSR Scaler writes helixsr.ini when the game starts and when the
// menu changes a setting; HelixSR's own sharpener is always off there. Other keys someone added to helixsr.ini
// (HelixSR's advanced settings) are kept.

struct Managed
{
    const char* section; // in helixsr.ini
    const char* key;
};
const Managed kManaged[] = { { "Upscaling", "Int8" },
                             { "Compatibility", "WaveSize" },
                             { "FrameGeneration", "QuickFlow" },
                             { "DlssFrameGeneration", "Preset" },
                             { "ModelE", "UseReactiveMask" } };
// earlier keys, taken out of helixsr.ini: sharpening (OptiScaler's now) and frame generation's old mode
const Managed kRetired[] = { { "Sharpening", "Sharpness" },
                             { "Sharpening", "UseGameSlider" },
                             { "FrameGeneration", "Mode" } };

fs::path HelixIniPath() { return folder / kIni; }

// Auto wave size, as HelixSR Scaler gives it to HelixSR: 32-lane network kernels on every GPU, as HelixSR 1.8.1 ran
// them. HelixSR 1.9.0's own auto runs them at 64 lanes on RDNA 3 and newer (RX 7000 / 9000, Ryzen with RDNA 3 / 3.5
// graphics), which was much slower on a Legion Go 2 (Z2 Extreme). NVIDIA, Intel and RDNA 1 / 2 get 32 from HelixSR's
// auto too; a GPU without 32-lane waves (Vega) still runs 64, HelixSR falls back by itself.
constexpr const char* kAutoWaveSize = "32";

// Frame generation quality from OptiScaler's config, nullopt for auto
std::optional<FgQuality::Preset> FgPreset()
{
    auto cfg = Config::Instance();
    if (!cfg->HelixSRFGPreset.has_value())
        return std::nullopt;
    return (FgQuality::Preset) std::clamp(cfg->HelixSRFGPreset.value(), 0, 1);
}

std::optional<FgQuality::QuickFlow> FgQuickFlow()
{
    auto cfg = Config::Instance();
    if (!cfg->HelixSRQuickFlow.has_value())
        return std::nullopt;
    return (FgQuality::QuickFlow) std::clamp(cfg->HelixSRQuickFlow.value(), 0, 2);
}

// The value of each managed key in helixsr.ini from OptiScaler's config, nullopt for auto (HelixSR's default)
std::optional<std::string> ConfigValue(const std::string& key)
{
    auto cfg = Config::Instance();
    if (key == "Int8" && cfg->HelixSRInt8.has_value())
        return cfg->HelixSRInt8.value();
    if (key == "WaveSize" && cfg->HelixSRWaveSize.has_value())
        return std::to_string(cfg->HelixSRWaveSize.value());
    // Frame generation quality (HelixSRFgQuality.h): Light (preset C, the default) or Full; QuickFlow only with Full,
    // so it stays off with Light (which also clears a Fast / Fastest left in OptiScaler.ini by an earlier version)
    if (key == "Preset")
        return std::string(FgQuality::HelixPreset(FgPreset()));
    if (key == "QuickFlow")
        return std::string(FgQuality::HelixQuickFlow(FgPreset(), FgQuickFlow()));
    // OptiScaler's "Disable Reactive Mask" (InitFlags): unticked gives the game's reactive mask to HelixSR's network
    // (its bias-current-colour input: particles, hair and other things without good motion vectors)
    if (key == "UseReactiveMask" && cfg->DisableReactiveMask.has_value())
        return std::string(cfg->DisableReactiveMask.value() ? "false" : "true");
    return std::nullopt;
}

const char* kHelixIniHeader[] = {
    "; HelixSR's settings, written by HelixSR Scaler from OptiScaler.ini each time the game starts. Change them in",
    "; OptiScaler.ini ([HelixSR]) or in the menu (Insert key). Sharpening is OptiScaler's, so HelixSR's sharpener",
    "; stays off. Int8, WaveSize, Preset and QuickFlow here are replaced; other HelixSR settings you add to this file",
    "; are kept.",
};

// helixsr.ini from OptiScaler's config
bool WriteHelixIni()
{
    auto ini = IniFile::Load(HelixIniPath());

    // our header in place of the comment lines at the top
    while (!ini.lines.empty() && (Trim(ini.lines.front()).empty() || Trim(ini.lines.front()).starts_with(";")))
        ini.lines.erase(ini.lines.begin());
    ini.lines.insert(ini.lines.begin(), "");
    for (size_t i = std::size(kHelixIniHeader); i-- > 0;)
        ini.lines.insert(ini.lines.begin(), kHelixIniHeader[i]);

    ini.Set("Sharpening", "Sharpener", "off");
    for (const auto& m : kRetired)
        ini.Set(m.section, m.key, std::nullopt);
    for (const auto& m : kManaged)
    {
        auto value = ConfigValue(m.key);
        if (!value.has_value() && std::string(m.key) == "WaveSize")
            value = kAutoWaveSize;

        // HelixSR's FidelityFX DLL reads its DLSS frame generation's settings from [DlssFrameGeneration]
        // ([FrameGeneration] is its FidelityFX frame generation's there)
        const char* section = !found && std::string(m.key) == "QuickFlow" ? "DlssFrameGeneration" : m.section;
        ini.Set(section, m.key, value);
    }

    return ini.Save(HelixIniPath());
}

// OptiScaler.ini's [HelixSR] lines from OptiScaler's config, without touching the rest
bool WriteOptiScalerIni()
{
    auto cfg = Config::Instance();
    const auto path = cfg->IniPath();
    auto ini = IniFile::Load(path);
    ini.separator = "=";

    for (const auto& m : kManaged)
    {
        // OptiScaler.ini keeps the choice as made (QuickFlow too with Light, for when Full is picked again)
        if (std::string(m.key) == "Preset")
            ini.Set("HelixSR", "FGPreset", std::string(FgQuality::OptiScalerPreset(FgPreset())));
        else if (std::string(m.key) == "QuickFlow")
            ini.Set("HelixSR", "QuickFlow", std::string(FgQuality::OptiScalerQuickFlow(FgQuickFlow())));
        else
            ini.Set("HelixSR", m.key, ConfigValue(m.key).value_or("auto"));
    }
    ini.Set("HelixSR", "FrameGeneration",
            cfg->HelixSRFrameGeneration.has_value() ? (cfg->HelixSRFrameGeneration.value() ? "on" : "off") : "auto");
    for (const char* key : { "Sharpener", "Sharpness", "UseGameSlider" }) // earlier versions' keys
        ini.Set("HelixSR", key, std::nullopt);

    return ini.Save(path);
}

// First start of a HelixSR Scaler with [HelixSR] in OptiScaler.ini: settings someone made in helixsr.ini move over
void ImportHelixIni()
{
    auto cfg = Config::Instance();
    const auto opti = IniFile::Load(cfg->IniPath());
    size_t b, e;
    if (opti.Range("HelixSR", b, e))
        return;

    std::error_code ec;
    if (fs::exists(HelixIniPath(), ec))
    {
        const auto old = IniFile::Load(HelixIniPath());
        if (auto v = old.Get("Upscaling", "Int8");
            v.has_value() && (Lower(v.value()) == "on" || Lower(v.value()) == "off"))
            cfg->HelixSRInt8 = Lower(v.value());
        if (auto v = old.Get("Compatibility", "WaveSize"); v.has_value() && (v.value() == "64" || v.value() == "32"))
            cfg->HelixSRWaveSize = std::stoi(v.value());
        if (auto v = old.Get(found ? "FrameGeneration" : "DlssFrameGeneration", "QuickFlow"); v.has_value())
        {
            if (auto q = FgQuality::ParseQuickFlow(v.value()); q.has_value())
                cfg->HelixSRQuickFlow = (int) q.value();
        }
        if (auto v = old.Get("DlssFrameGeneration", "Preset"); v.has_value())
        {
            if (auto p = FgQuality::ParsePreset(v.value()); p.has_value())
                cfg->HelixSRFGPreset = (int) p.value();
        }
        LOG_INFO("HelixSR Scaler: moved the settings in helixsr.ini to OptiScaler.ini");
    }

    WriteOptiScalerIni();
}

Settings ReadSettings()
{
    auto cfg = Config::Instance();
    Settings s;

    if (cfg->HelixSRInt8.has_value())
    {
        const auto x = Lower(cfg->HelixSRInt8.value());
        s.int8 = (x == "on" || x == "true" || x == "1") ? 1 : (x == "off" || x == "false" || x == "0") ? 2 : 0;
    }

    if (cfg->HelixSRWaveSize.has_value())
        s.wave = cfg->HelixSRWaveSize.value() == 64 ? 1 : cfg->HelixSRWaveSize.value() == 32 ? 2 : 0;

    s.frameGen = cfg->HelixSRFrameGeneration.value_or(false) ? 0 : 1; // auto is off

    return s;
}

bool WriteSettings(const Settings& s)
{
    auto cfg = Config::Instance();

    cfg->HelixSRInt8 = s.int8 == 1   ? std::optional<std::string>("on")
                       : s.int8 == 2 ? std::optional<std::string>("off")
                                     : std::optional<std::string>();
    cfg->HelixSRWaveSize = s.wave == 1   ? std::optional<int>(64)
                           : s.wave == 2 ? std::optional<int>(32)
                                         : std::optional<int>();
    // Off is the default (auto); On is saved
    cfg->HelixSRFrameGeneration = s.frameGen == 0 ? std::optional<bool>(true) : std::optional<bool>();

    const bool opti = WriteOptiScalerIni();
    const bool helix = WriteHelixIni();
    return opti && helix;
}

// --- helixsr.log ---------------------------------------------------------------------------------------------------

LogStatus ReadLog()
{
    LogStatus s;

    fs::path p = folder / kLog;
    std::error_code ec;
    if (!fs::exists(p, ec))
        p = Util::ExePath().parent_path() / kLog;

    std::ifstream in(p, std::ios::binary);
    if (!in)
        return s;

    // the end of the file is enough: the lines of the last start of the upscaler
    in.seekg(0, std::ios::end);
    const auto size = (long long) in.tellg();
    const long long keep = 512 * 1024;
    in.seekg(size > keep ? size - keep : 0);

    std::string line;
    while (std::getline(in, line))
    {
        if (auto at = line.find("summary: HelixSR "); at != std::string::npos)
        {
            auto v = line.substr(at + 17);
            s.version = Trim(v.substr(0, v.find('|')));
        }
        else if (auto at = line.find("int8 network: "); at != std::string::npos)
        {
            auto v = line.substr(at + 14);
            s.int8 = Trim(v.substr(0, v.find(' ')));
        }
        else if (auto at = line.find("network kernels wave"); at != std::string::npos)
        {
            s.networkWave = std::atoi(line.c_str() + at + 20);
        }

        if (line.find("network is missing from this DLL") != std::string::npos)
            s.networkMissing = true;
        else if (line.find("lacks shader model 6.6") != std::string::npos)
            s.gpuUnsupported = true;
        else if (line.find("network could not be started") != std::string::npos ||
                 line.find("network shaders unavailable") != std::string::npos)
            s.networkFailed = true;
        else if (line.find("network running") != std::string::npos)
            s.networkMissing = s.gpuUnsupported = s.networkFailed = false;

        if (line.find("frame generation NOT available on this GPU") != std::string::npos)
            s.fgUnsupported = true;
        else if (line.find("fg: GPU supports frame generation") != std::string::npos)
            s.fgUnsupported = false;
    }

    return s;
}

// --- Upscaler state ------------------------------------------------------------------------------------------------

::Upscaler Backend()
{
    auto& state = State::Instance();
    auto cfg = Config::Instance();

    if (state.api == DX11)
        return cfg->Dx11Upscaler.value_or_default();
    if (state.api == Vulkan)
        return cfg->VulkanUpscaler.value_or_default();
    return cfg->Dx12Upscaler.value_or_default();
}

// HelixSR is OptiScaler's DLSS upscaler on GPUs without DLSS (HelixSR's DLSS DLL as the NVNGX module); DirectX 11
// games run it through OptiScaler's Dx12 interop (DLSS w/Dx12, HelixSR's DLL is DirectX 12 only)
::Upscaler HelixSRUpscaler() { return State::Instance().api == DX11 ? ::Upscaler::DLSS_on12 : ::Upscaler::DLSS; }

void ChooseHelixSR() { ChooseBackend(HelixSRUpscaler()); }

void OpenFolder() { ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// --- Menu helpers --------------------------------------------------------------------------------------------------

ImVec4 (*toColor)(const ImVec4&) = nullptr;

ImVec4 Yellow() { return toColor(ImVec4(0.961f, 0.722f, 0.294f, 1.0f)); } // HelixSR amber
ImVec4 Red() { return toColor(ImVec4(0.949f, 0.478f, 0.478f, 1.0f)); }    // HelixSR danger

void Help(const char* text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// A row of buttons, one per choice; returns true when the choice changed
bool Choice(const char* label, int* value, const char* const* names, int count, float labelWidth)
{
    bool changed = false;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(labelWidth);

    ImGui::PushID(label);
    for (int i = 0; i < count; i++)
    {
        if (i > 0)
            ImGui::SameLine();

        const bool selected = *value == i;
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));

        if (ImGui::Button(names[i]) && !selected)
        {
            *value = i;
            changed = true;
        }

        if (selected)
            ImGui::PopStyleColor();
    }
    ImGui::PopID();

    return changed;
}

MenuState& Menu()
{
    static MenuState m;
    return m;
}

void RefreshMenu()
{
    auto& m = Menu();

    const double now = ImGui::GetTime();
    if (Installed() && now - m.lastRead > 2.0)
    {
        m.status = ReadLog();
        if (m.lastRead < 0.0)
            m.settings = m.saved = m.initial = ReadSettings();
        m.lastRead = now;
    }
}

void SaveMenuSettings()
{
    auto& m = Menu();
    // HelixSR's frame generation switched on: also back to the HelixSR route (the frame-generation settings on auto)
    if (m.settings.frameGen == 0 && m.saved.frameGen != 0)
        ApplyFgChoice(0);
    m.saveFailed = !WriteSettings(m.settings);
    if (!m.saveFailed)
        m.restartNeeded = m.settings.int8 != m.initial.int8 || m.settings.wave != m.initial.wave ||
                          m.settings.frameGen != m.initial.frameGen;
    m.saved = m.settings;
}

void ChooseBackend(::Upscaler upscaler)
{
    auto& state = State::Instance();

    state.newBackend = upscaler;
    for (auto& change : state.changeBackend)
        change.second = true;

    LOG_INFO("HelixSR Scaler: switching to {}", UpscalerToCode(upscaler));
}

} // namespace HelixSR::detail

namespace HelixSR
{
using namespace detail;

// [HelixSR] FrameGeneration: off (and auto, the default) renames the helixsr-fg folder to helixsr-fg.off before
// HelixSR's DLL loads, on renames it back. Runs at startup, before anything uses the folder.
static void SwitchFrameGeneration()
{
    auto cfg = Config::Instance();

    // HelixSR's last start found the GPU can't run its frame generation (no 32-lane waves: Vega, Polaris): the game
    // was told it is unavailable, but with OptiScaler's frame generation pointed at HelixSR it crashes (issue #53).
    // Switched off and saved in OptiScaler.ini, unless it says on.
    if (!cfg->HelixSRFrameGeneration.has_value() && ReadLog().fgUnsupported)
    {
        cfg->HelixSRFrameGeneration = false;
        WriteOptiScalerIni();
        LOG_WARN("HelixSR Scaler: this GPU can't run HelixSR's frame generation (helixsr.log: no 32-lane waves); "
                 "switched off, [HelixSR] FrameGeneration=off in OptiScaler.ini");
    }

    const bool off = !cfg->HelixSRFrameGeneration.value_or(false); // off unless switched on
    const auto on = folder / kFGFolder, parked = folder / kFGFolderOff;
    std::error_code ec;
    const bool hasOn = fs::is_directory(on, ec), hasParked = fs::is_directory(parked, ec);

    if (off && hasOn)
    {
        if (hasParked) // an install or update put a new helixsr-fg next to an older parked one
            fs::remove_all(parked, ec);
        fs::rename(on, parked, ec);
        if (ec)
            LOG_WARN("HelixSR Scaler: could not switch frame generation off ({})", ec.message());
        else
            LOG_INFO("HelixSR Scaler: frame generation off ([HelixSR] FrameGeneration={})",
                     cfg->HelixSRFrameGeneration.has_value() ? "off" : "auto");
    }
    else if (!off && hasParked)
    {
        if (hasOn) // the newer files are the ones in helixsr-fg
            fs::remove_all(parked, ec);
        else
            fs::rename(parked, on, ec);
        if (ec)
            LOG_WARN("HelixSR Scaler: could not switch frame generation on ({})", ec.message());
        else
            LOG_INFO("HelixSR Scaler: frame generation on");
    }
    else if (off)
    {
        LOG_INFO("HelixSR Scaler: frame generation off ([HelixSR] FrameGeneration={})",
                 cfg->HelixSRFrameGeneration.has_value() ? "off" : "auto");
    }
}

// The frame generation the game ships: files next to the .exe, else anywhere in the game's folder (the .exe's folder
// and up to three levels above it, searched a few levels down, e.g. Unreal Engine's plugin folders). OptiScaler's own
// folder and HelixSR's folders are left out (they carry FidelityFX and XeSS frame-generation DLLs of their own).
static FgRoute::Game ScanGame()
{
    FgRoute::Game game;
    std::error_code ec;
    const auto exeDir = Util::ExePath().parent_path();

    // up to three levels, not past a library folder (Steam's common) nor to a drive's root
    auto root = exeDir;
    for (int up = 0; up < 3; ++up)
    {
        const auto parent = root.parent_path();
        if (parent == root || parent.parent_path() == parent || _wcsicmp(parent.filename().c_str(), L"common") == 0)
            break;
        root = parent;
    }

    std::vector<fs::path> skip = { fs::path(Config::Instance()->MainDllPath.value_or(L"")).lexically_normal() };
    if (!folder.empty())
        skip.push_back(folder.lexically_normal());
    if (FsrDllFound())
        skip.push_back(FsrDllPath().parent_path().lexically_normal());

    auto note = [&game](const fs::path& file)
    {
        const auto name = file.filename().wstring();
        if (_wcsicmp(name.c_str(), L"sl.dlss_g.dll") == 0)
            game.dlssg = true;
        // not amd_fidelityfx_dx12.dll: also the upscaler-only FidelityFX DLL, and HelixSR's own when it replaces the
        // game's (a game with FSR 3.1 frame generation in it gets the upscaler route then)
        else if (_wcsicmp(name.c_str(), L"amd_fidelityfx_framegeneration_dx12.dll") == 0)
            game.fsrfg = true;
        else if (_wcsicmp(name.c_str(), L"ffx_frameinterpolation_x64.dll") == 0)
            game.fsrfg30 = true;
        else if (_wcsicmp(name.c_str(), L"libxess_fg.dll") == 0)
            game.xefg = true;
    };

    int seen = 0;
    auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator() && ++seen < 20000; it.increment(ec))
    {
        if (it.depth() > 7)
        {
            it.disable_recursion_pending();
            continue;
        }

        if (it->is_directory(ec))
        {
            const auto dir = it->path().lexically_normal();
            for (const auto& s : skip)
            {
                if (!s.empty() && _wcsicmp(dir.c_str(), s.c_str()) == 0)
                {
                    it.disable_recursion_pending();
                    break;
                }
            }
            continue;
        }

        if (it->is_regular_file(ec))
            note(it->path());
    }

    return game;
}

// HelixSR's colours for the menu (teal accent, ink background), only with HelixSR installed and unless
// OptiScaler.ini sets them; without HelixSR the menu keeps OptiScaler's colours
static void ApplyMenuTheme()
{
    if (!Installed())
        return;

    auto cfg = Config::Instance();
    const std::pair<CustomOptional<float>*, float> colours[] = {
        { &cfg->MenuAccentColorR, 0.176f }, { &cfg->MenuAccentColorG, 0.831f }, { &cfg->MenuAccentColorB, 0.749f },
        { &cfg->MenuBGColorR, 0.078f },     { &cfg->MenuBGColorG, 0.094f },     { &cfg->MenuBGColorB, 0.149f },
    };

    for (const auto& [option, value] : colours)
    {
        if (!option->has_value())
            option->set_volatile_value(value);
    }
}

static_assert((uint32_t) ::FGInput::XeFG == (uint32_t) FgRoute::Input::XeFG &&
              (uint32_t) ::FGInput::FSRFG30 == (uint32_t) FgRoute::Input::FSRFG30 &&
              (uint32_t) ::FGInput::Upscaler == (uint32_t) FgRoute::Input::Upscaler);
static_assert((uint32_t) ::FGOutput::Reprojection == (uint32_t) FgRoute::Output::Reprojection &&
              (uint32_t) ::FGOutput::DLSSG == (uint32_t) FgRoute::Output::DLSSG);
static_assert((uint32_t) ::FGNvngxReplacement::HelixSR == (uint32_t) FgRoute::Provider::HelixSR);

// What the game ships (ScanGame at startup), for the default route and the menu's frame-generation choice
static FgRoute::Game scannedGame;

// [HelixSR] FGPresent: optiscaler, streamline, or auto (anything else)
static FgRoute::Present PresentSetting()
{
    const auto v = Lower(Trim(Config::Instance()->HelixSRFGPresent.value_or("auto")));
    if (v == "optiscaler")
        return FgRoute::Present::OptiScaler;
    if (v == "streamline")
        return FgRoute::Present::Streamline;
    return FgRoute::Present::Auto;
}

bool FgPresentStreamline() { return PresentSetting() == FgRoute::Present::Streamline; }

// HelixSR's frame generation for every DirectX 12 game (HelixSRFgRoute.h): DLSS-G games through their own Streamline,
// the others through OptiScaler's FSR FG swapchain (the OptiFG route, FGOutput = fsrfg) with the inputs of the game's
// FSR / XeSS frame generation or of its upscaler. Only settings OptiScaler.ini leaves on auto are set, as volatile
// values (never saved).
static void ApplyFrameGenDefaults()
{
    auto cfg = Config::Instance();
    std::error_code ec;

    FgRoute::Settings s;
    s.engine = FrameGenFound();
    s.switchedOn = cfg->HelixSRFrameGeneration.value_or(false); // on only when the app or the user switched it on
    s.streamline =
        fs::is_regular_file(fs::path(cfg->MainDllPath.value_or(L"")) / L"streamline" / L"sl.interposer.dll", ec);
    s.present = PresentSetting();
    // a FidelityFX frame-generation DLL for the FSR FG swapchain: OptiScaler's, the one FfxDx12FGPath names, or the
    // game's (FSR 3.1 frame generation, checked after the scan)
    const auto ffxFgPath = cfg->FfxDx12FGPath.value_or(L"");
    s.ffxFg = fs::is_regular_file(fs::path(cfg->MainDllPath.value_or(L"")) / L"amd_fidelityfx_framegeneration_dx12.dll",
                                  ec) ||
              (!ffxFgPath.empty() && _wcsicmp(ffxFgPath.c_str(), L"auto") != 0 && fs::exists(ffxFgPath, ec));
    if (cfg->FGInput.has_value())
        s.input = (FgRoute::Input) cfg->FGInput.value();
    if (cfg->FGOutput.has_value())
        s.output = (FgRoute::Output) cfg->FGOutput.value();
    if (cfg->FGNvngxReplacement.has_value())
        s.provider = (FgRoute::Provider) cfg->FGNvngxReplacement.value();
    if (cfg->FGEnabled.has_value())
        s.enabled = cfg->FGEnabled.value();
    if (cfg->FGHUDFix.has_value())
        s.hudfix = cfg->FGHUDFix.value();

    // ForceXeLL (fakenvapi) owns the FG settings
    if (s.input == FgRoute::Input(::FGInput::ForceXeLL))
        return;

    scannedGame = ScanGame();
    LOG_INFO("HelixSR Scaler: the game has DLSS-G {}, FSR 3.1 FG {}, FSR 3.0 FG {}, XeSS FG {}", scannedGame.dlssg,
             scannedGame.fsrfg, scannedGame.fsrfg30, scannedGame.xefg);
    s.ffxFg = s.ffxFg || scannedGame.fsrfg;
    const auto route = FgRoute::Decide(scannedGame, s);

    if (route.input.has_value())
        cfg->FGInput.set_volatile_value((::FGInput) route.input.value());
    if (route.output.has_value())
        cfg->FGOutput.set_volatile_value((::FGOutput) route.output.value());
    if (route.provider.has_value())
        cfg->FGNvngxReplacement.set_volatile_value((::FGNvngxReplacement) route.provider.value());
    if (route.enabled.has_value())
        cfg->FGEnabled.set_volatile_value(route.enabled.value());
    if (route.hudfix.has_value())
        cfg->FGHUDFix.set_volatile_value(route.hudfix.value());

    LOG_INFO("HelixSR Scaler: frame generation: {} (FGPresent = {})", route.why,
             s.present == FgRoute::Present::OptiScaler   ? "optiscaler"
             : s.present == FgRoute::Present::Streamline ? "streamline"
                                                         : "auto");
    LOG_INFO("HelixSR Scaler: frame generation settings: input={}, output={}, provider={}, enabled={}, hudfix={} "
             "(DirectX 12 only: DirectX 11 and Vulkan games get no HelixSR frame generation)",
             magic_enum::enum_name(cfg->FGInput.value_or_default()),
             magic_enum::enum_name(cfg->FGOutput.value_or_default()),
             magic_enum::enum_name(cfg->FGNvngxReplacement.value_or_default()), cfg->FGEnabled.value_or_default(),
             cfg->FGHUDFix.value_or_default());
}

void ApplyDefaults()
{
    auto cfg = Config::Instance();
    std::error_code ec;

    std::vector<fs::path> places = { Util::DllPath().parent_path() / kFolder,
                                     fs::path(cfg->MainDllPath.value_or(L"")) / kFolder,
                                     Util::ExePath().parent_path() / kFolder };

    for (const auto& p : places)
    {
        if (fs::exists(p / kDll, ec))
        {
            folder = fs::absolute(p, ec);
            found = true;
            break;
        }
    }

    if (found)
    {
        LOG_INFO("HelixSR Scaler {}: HelixSR found in {}", Version(), wstring_to_string(folder.wstring()));
    }
    else
    {
        // HelixSR's FidelityFX DLL as the FFX upscaler instead (HelixSRFsrDll.cpp)
        ApplyFsrDllDefaults();

        if (!FsrDllFound())
        {
            LOG_WARN("HelixSR Scaler: no {} folder with {} and no HelixSR-fsrfg\\amd_fidelityfx_dx12.dll next to this "
                     "DLL or the game's .exe",
                     wstring_to_string(kFolder), wstring_to_string(kDll));
            return;
        }

        // HelixSR's settings (helixsr.ini), its log, its frame generation (helixsr-fg) and its startup markers are
        // next to its FidelityFX DLL
        folder = FsrDllPath().parent_path();
        LOG_INFO("HelixSR Scaler {}: HelixSR's FidelityFX DLL in {} (no helixsr.dll)", Version(),
                 wstring_to_string(folder.wstring()));
    }

    ApplyMenuTheme();

    // HelixSR's settings live in OptiScaler.ini [HelixSR]; helixsr.ini is written from them for HelixSR
    ImportHelixIni();
    if (!WriteHelixIni())
        LOG_WARN("HelixSR Scaler: could not write {}", wstring_to_string(HelixIniPath().wstring()));

    SwitchFrameGeneration();

    if (found)
    {
        // HelixSR is the DLSS upscaler (on GPUs without DLSS, see NVNGXProxy::HelixSRDlss); DirectX 11 games run it
        // through the Dx12 interop (FeatureProvider_Dx11: DLSS w/Dx12)
        if (!cfg->Dx12Upscaler.has_value())
            cfg->Dx12Upscaler.set_volatile_value(::Upscaler::DLSS);

        if (!cfg->Dx11Upscaler.has_value())
            cfg->Dx11Upscaler.set_volatile_value(::Upscaler::DLSS);
    }

    ApplyFrameGenDefaults();
}

bool Found() { return found; }

// HelixSR's frame generation is installed and usable this launch: the helixsr-fg folder next to HelixSR's DLL
// (helixsr.dll, or HelixSR's FidelityFX DLL without one), not switched off ([HelixSR] FrameGeneration = off parks it as
// helixsr-fg.off)
bool FrameGenFound()
{
    std::error_code ec;
    return Installed() && !folder.empty() && fs::is_directory(folder / kFGFolder, ec);
}

// The helixsr-fg folder is there, switched on or off
bool detail::FrameGenInstalled()
{
    std::error_code ec;
    return Installed() && !folder.empty() &&
           (fs::is_directory(folder / kFGFolder, ec) || fs::is_directory(folder / kFGFolderOff, ec));
}

fs::path DllPath() { return found ? folder / kDll : fs::path(); }

// The folders of the FidelityFX DLL serving DLSS frame generation (LoadFfxFrameGenDll): as configured and as the
// loader names it (the drive can differ under Wine); empty while there is none
static std::vector<fs::path> ffxFgFolders;
static std::mutex ffxFgMutex;

HMODULE LoadFfxFrameGenDll()
{
    const auto configured = Config::Instance()->FfxDx12SRPath.value_or(L"");
    if (configured.empty() || _wcsicmp(configured.c_str(), L"auto") == 0)
    {
        LOG_INFO("HelixSR Scaler: no helixsr.dll and no [Libraries] FfxDx12SRPath: no HelixSR DLL for DLSS frame "
                 "generation");
        return nullptr;
    }

    std::error_code ec;
    fs::path path(configured);
    if (fs::is_directory(path, ec))
        path /= L"amd_fidelityfx_dx12.dll";
    if (!fs::is_regular_file(path, ec))
    {
        LOG_WARN("HelixSR Scaler: FfxDx12SRPath {} not found: no HelixSR DLL for DLSS frame generation",
                 wstring_to_string(path.wstring()));
        return nullptr;
    }

    HMODULE dll = NtdllProxy::LoadLibraryExW_Ldr(path.c_str(), NULL, 0);
    using VersionFn = uint32_t (*)();
    auto version = dll != nullptr ? (VersionFn) GetProcAddress(dll, "HelixSR_NgxFgVersion") : nullptr;
    if (version == nullptr || version() < 1)
    {
        LOG_WARN("HelixSR Scaler: {} {}: no HelixSR DLL for DLSS frame generation", wstring_to_string(path.wstring()),
                 dll == nullptr ? "can't be loaded" : "doesn't serve DLSS frame generation (no HelixSR_NgxFgVersion)");
        if (dll != nullptr)
            FreeLibrary(dll);
        return nullptr;
    }

    wchar_t loaded[MAX_PATH * 2] = {};
    GetModuleFileNameW(dll, loaded, MAX_PATH * 2);
    {
        std::lock_guard<std::mutex> lock(ffxFgMutex);
        ffxFgFolders = { fs::absolute(path, ec).parent_path(), fs::path(loaded).parent_path() };
    }

    LOG_INFO("HelixSR Scaler: no helixsr.dll: DLSS frame generation by HelixSR's FidelityFX DLL {} (NGX frame "
             "generation version {})",
             wstring_to_string(loaded), version());
    return dll;
}

bool LoadsAsIs(const fs::path& path)
{
    if (found && Util::IsSubpath(path, folder))
        return true;

    std::lock_guard<std::mutex> lock(ffxFgMutex);
    for (const auto& f : ffxFgFolders)
    {
        if (!f.empty() && Util::IsSubpath(path, f))
            return true;
    }
    return false;
}

const fs::path& Folder() { return folder; }

const char* Version() { return HELIXSR_SCALER_VERSION_STR; }

void Choose() { ChooseHelixSR(); }

// --- The menu's frame-generation choice: HelixSR / AMD FSR / Intel XeSS / Off ------------------------------------
// Saved to OptiScaler.ini; it applies at the next game start (OptiScaler sets its frame generation up at the start).
// OptiScaler's own input / output / provider lists stay under it for other combinations.

enum FgChoice : int
{
    FgChoiceHelixSR,
    FgChoiceAmd,
    FgChoiceIntel,
    FgChoiceOff,
    FgChoiceCustom, // another combination, from OptiScaler's own lists
};

// What runs now
static int RunningFgChoice()
{
    const auto& state = State::Instance();

    if (state.activeFgNvngx == ::FGNvngxReplacement::HelixSR)
        return FgChoiceHelixSR;
    if (state.activeFgOutput == ::FGOutput::XeFG)
        return FgChoiceIntel;
    if (state.activeFgOutput == ::FGOutput::FSRFG ||
        (state.activeFgInput == ::FGInput::NvngxFG && state.activeFgNvngx == ::FGNvngxReplacement::FFX))
        return FgChoiceAmd;
    if (state.activeFgInput == ::FGInput::NoFG)
        return FgChoiceOff;
    return FgChoiceCustom;
}

// The input of the game's own frame generation (or the upscaler's), for AMD's and Intel's frame generation
static ::FGInput GameFgInput()
{
    if (scannedGame.dlssg)
        return ::FGInput::DLSSG;
    if (scannedGame.fsrfg)
        return ::FGInput::FSRFG;
    if (scannedGame.fsrfg30)
        return ::FGInput::FSRFG30;
    if (scannedGame.xefg)
        return ::FGInput::XeFG;
    return ::FGInput::Upscaler;
}

void detail::ApplyFgChoice(int choice)
{
    auto cfg = Config::Instance();

    // HelixSR's choice switches HelixSR's frame generation on and leaves the frame-generation settings on auto (HelixSR
    // Scaler picks the route by the game); the others switch it off ([HelixSR] FrameGeneration = off parks its folder
    // at the next start)
    cfg->HelixSRFrameGeneration = std::optional<bool>(choice == FgChoiceHelixSR);
    cfg->FGInput = std::optional<::FGInput>();
    cfg->FGOutput = std::optional<::FGOutput>();
    cfg->FGNvngxReplacement = std::optional<::FGNvngxReplacement>();
    cfg->FGEnabled = std::optional<bool>();

    const auto input = GameFgInput();

    if (choice == FgChoiceAmd && input == ::FGInput::DLSSG)
    {
        // DLSS-G games: the game's DLSS frame generation runs AMD's FSR frame generation (OptiScaler's FFX provider)
        cfg->FGInput = ::FGInput::NvngxFG;
        cfg->FGNvngxReplacement = ::FGNvngxReplacement::FFX;
    }
    else if (choice == FgChoiceAmd || choice == FgChoiceIntel)
    {
        cfg->FGInput = input;
        cfg->FGOutput = choice == FgChoiceAmd ? ::FGOutput::FSRFG : ::FGOutput::XeFG;
        if (input != ::FGInput::DLSSG)
            cfg->FGEnabled = true;
        if (input == ::FGInput::Upscaler && !cfg->FGHUDFix.has_value())
            cfg->FGHUDFix = true;
    }
    else if (choice == FgChoiceOff)
    {
        cfg->FGInput = ::FGInput::NoFG;
        cfg->FGOutput = ::FGOutput::NoFG;
    }

    const bool saved = cfg->SaveIni() && WriteOptiScalerIni();
    WriteHelixIni();
    LOG_INFO("HelixSR Scaler: frame generation {} chosen in the menu, saved {} (applies at the next game start)",
             choice == FgChoiceHelixSR ? "HelixSR"
             : choice == FgChoiceAmd   ? "AMD FSR"
             : choice == FgChoiceIntel ? "Intel XeSS"
                                       : "off",
             saved);
}

void RenderFrameGenChoice(float scale)
{
    if (!Installed())
        return;

    static int chosen = -1; // picked in this session (the restart note)
    const int running = RunningFgChoice();
    const int shown = chosen >= 0 ? chosen : running;

    const char* names[] = { "HelixSR", "AMD FSR", "Intel XeSS", "Off", "Custom" };

    ImGui::SetNextItemWidth(150.0f * scale);
    if (ImGui::BeginCombo("Frame generation", names[shown]))
    {
        for (int i = FgChoiceHelixSR; i <= FgChoiceOff; i++)
        {
            const bool disabled = i == FgChoiceHelixSR && !FrameGenInstalled();
            if (disabled)
                ImGui::BeginDisabled();
            if (ImGui::Selectable(names[i], shown == i) && shown != i)
            {
                ApplyFgChoice(i);
                chosen = i;
            }
            if (disabled)
                ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("HelixSR: HelixSR's frame generation (DirectX 12 games).\nAMD FSR: AMD's FSR frame "
                          "generation.\nIntel XeSS: Intel's XeSS frame generation.\nOff: no frame generation.\n\n"
                          "Saved to OptiScaler.ini, applies after restarting the game. The input / output lists below "
                          "are for other combinations.");

    if (chosen >= 0 && chosen != running)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Restart the game to apply");
    else if (running == FgChoiceHelixSR && State::Instance().api != DX12 && State::Instance().api != NotSelected)
        ImGui::TextDisabled("HelixSR frame generation runs in DirectX 12 games only");
}

void RenderFrameGenQuality(float scale)
{
    if (!FrameGenFound())
        return;

    auto cfg = Config::Instance();

    // what the game started with (the restart note)
    static bool atStartSet = false;
    static FgQuality::Preset presetAtStart = FgQuality::Preset::Light;
    static FgQuality::QuickFlow quickFlowAtStart = FgQuality::QuickFlow::Off;
    if (!atStartSet)
    {
        presetAtStart = FgQuality::EffectivePreset(FgPreset());
        quickFlowAtStart = FgQuality::EffectiveQuickFlow(FgPreset(), FgQuickFlow());
        atStartSet = true;
    }

    bool changed = false;
    const auto preset = FgQuality::EffectivePreset(FgPreset());
    const char* presets[] = { "Light (preset C)", "Full" };
    int pick = (int) preset;

    ImGui::SetNextItemWidth(150.0f * scale);
    if (ImGui::BeginCombo("Frame generation quality", presets[pick]))
    {
        for (int i = 0; i < 2; i++)
        {
            if (ImGui::Selectable(presets[i], i == pick) && i != pick)
            {
                cfg->HelixSRFGPreset = i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Light: NVIDIA's light mode (preset C), the default: faster.\nFull: NVIDIA's full frame "
                          "generation, with QuickFlow to choose.\n\nApplies after restarting the game.");

    if (FgQuality::EffectivePreset(FgPreset()) == FgQuality::Preset::Full)
    {
        const char* flows[] = { "Off", "Fast", "Fastest" };
        int flow = (int) FgQuickFlow().value_or(FgQuality::QuickFlow::Off);

        ImGui::SetNextItemWidth(150.0f * scale);
        if (ImGui::BeginCombo("QuickFlow", flows[flow]))
        {
            for (int i = 0; i < 3; i++)
            {
                if (ImGui::Selectable(flows[i], i == flow) && i != flow)
                {
                    cfg->HelixSRQuickFlow = i;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off: full quality.\nFast: QuickFlow, a faster motion estimate for lower-end GPUs, "
                              "slightly softer in-between frames.\nFastest: QuickFlow at half size, upscaled with "
                              "detail restore. The fastest; it can shimmer (mostly at 4K).\n\nApplies after "
                              "restarting the game.");
    }

    if (changed)
    {
        const bool opti = WriteOptiScalerIni();
        const bool helix = WriteHelixIni();
        LOG_INFO("HelixSR Scaler: frame generation Preset = {}, QuickFlow = {} saved (OptiScaler.ini {}, helixsr.ini "
                 "{}); it applies at the next game start",
                 FgQuality::HelixPreset(FgPreset()), FgQuality::HelixQuickFlow(FgPreset(), FgQuickFlow()), opti, helix);
    }

    if (FgQuality::EffectivePreset(FgPreset()) != presetAtStart ||
        FgQuality::EffectiveQuickFlow(FgPreset(), FgQuickFlow()) != quickFlowAtStart)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Restart the game to apply");
}

bool IsActive()
{
    auto& state = State::Instance();
    auto feature = state.currentFeature;
    if (feature == nullptr || !feature->IsInited())
        return false;

    const auto type = feature->GetUpscalerType();
    return ((type == ::Upscaler::DLSS || type == ::Upscaler::DLSS_on12) && NVNGXProxy::HelixSRDlss()) || IsActiveFfx();
}

void RenderSettings(float scale, ImVec4 (*color)(const ImVec4&))
{
    toColor = color;
    RefreshMenu();

    if (!Installed())
        return;

    auto& m = Menu();
    auto& settings = m.settings;
    const auto& status = m.status;
    const float labelWidth = 130.0f * scale;
    bool changed = false;

    changed |= Choice("Speed boost", &settings.int8, kInt8, kInt8Count, labelWidth);
    Help(kInt8Help);

    if (!status.int8.empty())
    {
        ImGui::SetCursorPosX(labelWidth);
        ImGui::TextDisabled("Right now: %s", status.int8 == "on" ? "on" : "off");
    }

    changed |= Choice("GPU mode", &settings.wave, kWave, kWaveCount, labelWidth);
    Help(kWaveHelp);

    if (FrameGenInstalled())
    {
        changed |= Choice("Frame generation", &settings.frameGen, kFrameGen, kFrameGenCount, labelWidth);
        Help(kFrameGenHelp);
    }

    if (status.networkWave > 0)
    {
        ImGui::SetCursorPosX(labelWidth);
        ImGui::TextDisabled("Right now: %d", status.networkWave);
    }

    if (ImGui::Button("Open the HelixSR folder"))
        OpenFolder();
    Help("helixsr.log in this folder tells what HelixSR did. Attach it when you report a problem.");

    if (changed)
        SaveMenuSettings();

    if (m.saveFailed)
        ImGui::TextColored(Red(), "Couldn't save helixsr.ini in the HelixSR folder.");
    else if (m.restartNeeded)
        ImGui::TextColored(Yellow(), "Saved. Restart the game to use the new settings.");

    ImGui::TextDisabled("HelixSR %s, HelixSR Scaler %s", status.version.empty() ? "?" : status.version.c_str(),
                        Version());
}
} // namespace HelixSR
