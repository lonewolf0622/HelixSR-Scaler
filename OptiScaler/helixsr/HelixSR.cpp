#include "pch.h"

#include "HelixSR.h"
#include "HelixSRInternal.h"

#include "Config.h"
#include "State.h"
#include "Util.h"
#include "resource.h"
#include "proxies/NVNGX_Proxy.h"

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

// The value of each managed key from OptiScaler's config, nullopt for auto (HelixSR's default)
std::optional<std::string> ConfigValue(const std::string& key)
{
    auto cfg = Config::Instance();
    if (key == "Int8" && cfg->HelixSRInt8.has_value())
        return cfg->HelixSRInt8.value();
    if (key == "WaveSize" && cfg->HelixSRWaveSize.has_value())
        return std::to_string(cfg->HelixSRWaveSize.value());
    if (key == "QuickFlow" && cfg->HelixSRQuickFlow.has_value())
    {
        const int qf = cfg->HelixSRQuickFlow.value();
        return std::string(qf == 2 ? "fastest" : qf == 1 ? "on" : "off");
    }
    // OptiScaler's "Disable Reactive Mask" (InitFlags): unticked gives the game's reactive mask to HelixSR's network
    // (its bias-current-colour input: particles, hair and other things without good motion vectors)
    if (key == "UseReactiveMask" && cfg->DisableReactiveMask.has_value())
        return std::string(cfg->DisableReactiveMask.value() ? "false" : "true");
    return std::nullopt;
}

const char* kHelixIniHeader[] = {
    "; HelixSR's settings, written by HelixSR Scaler from OptiScaler.ini each time the game starts. Change them in",
    "; OptiScaler.ini ([HelixSR]) or in the menu (Insert key). Sharpening is OptiScaler's, so HelixSR's sharpener",
    "; stays off. Int8, WaveSize and QuickFlow here are replaced; other HelixSR settings you add to this file are",
    "; kept.",
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
        ini.Set(m.section, m.key, value);
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
        ini.Set("HelixSR", m.key, ConfigValue(m.key).value_or("auto"));
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
        if (auto v = old.Get("FrameGeneration", "QuickFlow"); v.has_value())
        {
            const auto qf = Lower(v.value());
            if (qf == "on" || qf == "off" || qf == "fastest")
                cfg->HelixSRQuickFlow = qf == "fastest" ? 2 : qf == "on" ? 1 : 0;
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

    s.frameGen = cfg->HelixSRFrameGeneration.value_or(true) ? 0 : 1;

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
    // On is the default (auto); Off is saved
    cfg->HelixSRFrameGeneration = s.frameGen == 1 ? std::optional<bool>(false) : std::optional<bool>();

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
    if (found && now - m.lastRead > 2.0)
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

// [HelixSR] FrameGeneration: off renames the helixsr-fg folder to helixsr-fg.off before HelixSR's DLL loads, on (and
// auto) renames it back. Runs at startup, before anything uses the folder.
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

    const bool off = cfg->HelixSRFrameGeneration.has_value() && !cfg->HelixSRFrameGeneration.value();
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
            LOG_INFO("HelixSR Scaler: frame generation off ([HelixSR] FrameGeneration=off)");
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
        LOG_INFO("HelixSR Scaler: frame generation off ([HelixSR] FrameGeneration=off)");
    }
}

// Streamline's DLSS Frame Generation plugin in the game: next to the .exe, else anywhere in the game's folder (the
// .exe's folder and up to three levels above it, searched a few levels down, e.g. Unreal Engine's plugin folders)
static bool GameHasDlssg()
{
    std::error_code ec;
    const auto exeDir = Util::ExePath().parent_path();
    if (fs::exists(exeDir / L"sl.dlss_g.dll", ec))
        return true;

    // up to three levels, not past a library folder (Steam's common) nor to a drive's root
    auto root = exeDir;
    for (int up = 0; up < 3; ++up)
    {
        const auto parent = root.parent_path();
        if (parent == root || parent.parent_path() == parent || _wcsicmp(parent.filename().c_str(), L"common") == 0)
            break;
        root = parent;
    }

    int seen = 0;
    auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator() && ++seen < 20000; it.increment(ec))
    {
        if (it.depth() > 7)
        {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && _wcsicmp(it->path().filename().c_str(), L"sl.dlss_g.dll") == 0)
            return true;
    }
    return false;
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

    if (!found)
    {
        LOG_WARN("HelixSR Scaler: no {} folder with {} next to this DLL or the game's .exe", wstring_to_string(kFolder),
                 wstring_to_string(kDll));

        // HelixSR's FidelityFX DLL as the FFX upscaler instead (HelixSRFsrDll.cpp)
        ApplyFsrDllDefaults();
        return;
    }

    LOG_INFO("HelixSR Scaler {}: HelixSR found in {}", Version(), wstring_to_string(folder.wstring()));

    // HelixSR's settings live in OptiScaler.ini [HelixSR]; helixsr.ini is written from them for HelixSR
    ImportHelixIni();
    if (!WriteHelixIni())
        LOG_WARN("HelixSR Scaler: could not write {}", wstring_to_string(HelixIniPath().wstring()));

    // Only HelixSR's provider is withdrawn. Preserve the user's FG input, output,
    // enabled state and other providers so FSR, XeSS and external mods still work.
    cfg->HelixSRFrameGeneration.set_volatile_value(false);
    if (cfg->FGNvngxReplacement.value_or_default() == ::FGNvngxReplacement::HelixSR)
    {
        cfg->FGNvngxReplacement.set_volatile_value(::FGNvngxReplacement::FFX);
        LOG_WARN("HelixSR frame generation is not included; using the FFX provider instead.");
    }
    if (!cfg->Dx12Upscaler.has_value())
        cfg->Dx12Upscaler.set_volatile_value(::Upscaler::DLSS);

    // DirectX 11: DLSS is HelixSR through the Dx12 interop (FeatureProvider_Dx11 runs it as DLSS w/Dx12), or the
    // native DLSS on GPUs with DLSS. Frame generation stays DirectX 12 only.
    if (!cfg->Dx11Upscaler.has_value())
        cfg->Dx11Upscaler.set_volatile_value(::Upscaler::DLSS);

    LOG_INFO("HelixSR frame generation is not included; alternative FG settings: input={}, output={}, provider={}",
             magic_enum::enum_name(cfg->FGInput.value_or_default()),
             magic_enum::enum_name(cfg->FGOutput.value_or_default()),
             magic_enum::enum_name(cfg->FGNvngxReplacement.value_or_default()));
}

bool Found() { return found; }

bool FrameGenFound() { return false; }

bool detail::FrameGenInstalled() { return false; }

fs::path DllPath() { return found ? folder / kDll : fs::path(); }

const fs::path& Folder() { return folder; }

const char* Version() { return HELIXSR_SCALER_VERSION_STR; }

void Choose() { ChooseHelixSR(); }

// The QuickFlow setting the game started with (restart note): 0 Best, 1 Fast, 2 Fastest
int quickFlowAtStart = 0;
bool quickFlowAtStartSet = false;

void RenderQuickFlow()
{
    if (!FrameGenFound())
        return;

    auto cfg = Config::Instance();
    const int now = std::clamp(cfg->HelixSRQuickFlow.value_or(0), 0, 2);

    if (!quickFlowAtStartSet)
    {
        quickFlowAtStart = now;
        quickFlowAtStartSet = true;
    }

    const char* names[] = { "Best", "Fast", "Fastest" };
    int pick = now;

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::BeginCombo("Frame generation quality", names[pick]))
    {
        for (int i = 0; i < 3; i++)
        {
            if (ImGui::Selectable(names[i], i == pick))
                pick = i;
        }
        ImGui::EndCombo();
    }

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Best: full quality.\nFast: QuickFlow, a faster motion estimate for lower-end GPUs, slightly "
            "softer in-between frames.\nFastest: QuickFlow at half size, upscaled with detail restore. The "
            "fastest; it can shimmer (mostly at 4K).\n\nApplies after restarting the game.");

    if (pick != now)
    {
        cfg->HelixSRQuickFlow = pick;
        const bool opti = WriteOptiScalerIni();
        const bool helix = WriteHelixIni();
        LOG_INFO(
            "HelixSR Scaler: QuickFlow = {} saved (OptiScaler.ini {}, helixsr.ini {}); it applies at the next game "
            "start",
            pick == 2   ? "fastest"
            : pick == 1 ? "on"
                        : "off",
            opti, helix);
    }

    if (std::clamp(cfg->HelixSRQuickFlow.value_or(0), 0, 2) != quickFlowAtStart)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Restart the game to use the new quality");
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

    if (!found)
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
