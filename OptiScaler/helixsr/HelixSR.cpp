#include "pch.h"

#include "HelixSR.h"
#include "HelixSRInternal.h"

#include "Config.h"
#include "State.h"
#include "Util.h"
#include "resource.h"

#include <shellapi.h>

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
const wchar_t* kDll = L"helixsr.dll";            // FfxDx12Path
const wchar_t* kDllSR = L"helixsr_upscaler.dll"; // FfxDx12SRPath: must be a second file, see CopyIfChanged
const wchar_t* kFG = L"amd_fidelityfx_framegeneration_dx12.dll"; // AMD's frame generation, HelixSR has none
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

// OptiScaler hooks the upscaler module, so HelixSR has to be two files: one for FfxDx12Path and one for
// FfxDx12SRPath (one file in both roles hangs the game when the upscaler starts). The second one is a copy, kept
// up to date here so updating HelixSR means replacing one file.
void CopyIfChanged(const fs::path& from, const fs::path& to)
{
    std::error_code ec;
    const bool same = fs::exists(to, ec) && fs::file_size(from, ec) == fs::file_size(to, ec) &&
                      fs::last_write_time(to, ec) >= fs::last_write_time(from, ec);
    if (same)
        return;

    if (CopyFileW(from.c_str(), to.c_str(), FALSE))
        LOG_INFO("HelixSR Scaler: copied {} to {}", wstring_to_string(from.filename().wstring()),
                 wstring_to_string(to.filename().wstring()));
    else
        LOG_WARN("HelixSR Scaler: could not copy {} to {} (error {})", wstring_to_string(from.wstring()),
                 wstring_to_string(to.wstring()), GetLastError());
}

// --- helixsr.ini ------------------------------------------------------------------------------------------------
// Line based, so the comments and examples in the file stay as they are.

struct IniFile
{
    std::vector<std::string> lines;

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
            lines.push_back(key + " = " + value.value());
            return;
        }

        for (size_t i = b; i < e; i++)
        {
            if (KeyOf(lines[i], false) != Lower(key))
                continue;

            if (value.has_value())
            {
                lines[i] = key + " = " + value.value();
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
        lines.insert(lines.begin() + at, key + " = " + value.value());
    }
};

// --- Settings shown in the menu ------------------------------------------------------------------------------------

extern const char* const kSharpness[] = { "Default", "Off", "Low", "Medium", "High" };
const float kSharpnessValue[] = { 0.0f, 0.0f, 0.1f, 0.3f, 0.5f };
extern const char* const kInt8[] = { "Auto", "On", "Off" };
extern const char* const kWave[] = { "Auto", "64", "32" };

fs::path IniPath() { return folder / kIni; }

Settings ReadSettings()
{
    Settings s;
    auto ini = IniFile::Load(IniPath());

    if (auto v = ini.Get("Sharpening", "Sharpener"); v.has_value())
    {
        auto name = Lower(v.value());
        if (name == "off" || name == "none" || name == "false" || name == "0")
        {
            s.sharpness = 1;
        }
        else
        {
            s.sharpener = name;
            float amount = 0.1f;
            if (auto a = ini.Get("Sharpening", "Sharpness"); a.has_value())
            {
                try
                {
                    amount = std::stof(a.value());
                }
                catch (...)
                {
                }
            }
            s.sharpness = amount <= 0.0f ? 1 : amount <= 0.15f ? 2 : amount <= 0.4f ? 3 : 4;
        }
    }

    if (auto v = ini.Get("Upscaling", "Int8"); v.has_value())
    {
        auto x = Lower(v.value());
        s.int8 = (x == "on" || x == "true" || x == "1") ? 1 : (x == "off" || x == "false" || x == "0") ? 2 : 0;
    }

    if (auto v = ini.Get("Compatibility", "WaveSize"); v.has_value())
        s.wave = v.value() == "64" ? 1 : v.value() == "32" ? 2 : 0;

    return s;
}

bool WriteSettings(const Settings& s)
{
    auto ini = IniFile::Load(IniPath());
    if (ini.lines.empty())
        ini.lines = { "; HelixSR settings. HelixSR reads this file next to its DLL.",
                      "; Changed by HelixSR Scaler's menu (Insert key in the game)." };

    if (s.sharpness == 0)
    {
        ini.Set("Sharpening", "Sharpener", std::nullopt);
        ini.Set("Sharpening", "Sharpness", std::nullopt);
    }
    else if (s.sharpness == 1)
    {
        ini.Set("Sharpening", "Sharpener", "off");
        ini.Set("Sharpening", "Sharpness", std::nullopt);
    }
    else
    {
        const bool known =
            s.sharpener == "nvidia" || s.sharpener == "nis" || s.sharpener == "rcas" || s.sharpener == "amd";
        ini.Set("Sharpening", "Sharpener", known ? s.sharpener : "nvidia");
        ini.Set("Sharpening", "Sharpness", std::format("{:.1f}", kSharpnessValue[s.sharpness]));
    }

    ini.Set("Upscaling", "Int8",
            s.int8 == 1   ? std::optional<std::string>("on")
            : s.int8 == 2 ? std::optional<std::string>("off")
                          : std::nullopt);

    ini.Set("Compatibility", "WaveSize",
            s.wave == 1   ? std::optional<std::string>("64")
            : s.wave == 2 ? std::optional<std::string>("32")
                          : std::nullopt);

    return ini.Save(IniPath());
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
    }

    return s;
}

// --- Upscaler state ------------------------------------------------------------------------------------------------

std::string BackendCode()
{
    auto& state = State::Instance();
    auto cfg = Config::Instance();

    if (state.api == DX11)
        return cfg->Dx11Upscaler.value_or_default();
    if (state.api == Vulkan)
        return cfg->VulkanUpscaler.value_or_default();
    return cfg->Dx12Upscaler.value_or_default();
}

// HelixSR's place in the FidelityFX upscaler list; HelixSR lists itself first, so 0 before the list is known
int FfxIndex()
{
    const auto& names = State::Instance().ffxUpscalerVersionNames;
    for (size_t i = 0; i < names.size(); i++)
    {
        if (names[i] != nullptr && std::strstr(names[i], "HelixSR") != nullptr)
            return (int) i;
    }
    return 0;
}

void ChooseHelixSR()
{
    auto& state = State::Instance();
    auto cfg = Config::Instance();

    cfg->FfxUpscalerIndex = FfxIndex();
    state.newBackend = state.api == DX12 ? "fsr31" : "fsr31_12";

    for (auto& change : state.changeBackend)
        change.second = true;

    LOG_INFO("HelixSR Scaler: switching to HelixSR ({})", state.newBackend);
}

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
        m.restartNeeded = m.settings.sharpness != m.initial.sharpness || m.settings.int8 != m.initial.int8 ||
                          m.settings.wave != m.initial.wave;
    m.saved = m.settings;
}

void ChooseBackend(const std::string& code)
{
    auto& state = State::Instance();
    auto cfg = Config::Instance();

    // FSR 3.X has to leave HelixSR's place in the FidelityFX list
    if (code == "fsr31" || code == "fsr31_12")
    {
        const auto& names = state.ffxUpscalerVersionNames;
        for (size_t i = 0; i < names.size(); i++)
        {
            if (names[i] != nullptr && std::strstr(names[i], "HelixSR") == nullptr)
            {
                cfg->FfxUpscalerIndex = (int) i;
                break;
            }
        }
    }

    state.newBackend = code;
    for (auto& change : state.changeBackend)
        change.second = true;

    LOG_INFO("HelixSR Scaler: switching to {}", code);
}

} // namespace HelixSR::detail

namespace HelixSR
{
using namespace detail;

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
        return;
    }

    LOG_INFO("HelixSR Scaler {}: HelixSR found in {}", Version(), wstring_to_string(folder.wstring()));

    CopyIfChanged(folder / kDll, folder / kDllSR);

    // OptiScaler.ini wins when it sets these; set_volatile_value keeps the paths out of OptiScaler.ini when it is
    // saved, so the game folder can be moved
    if (!cfg->FfxDx12Path.has_value())
        cfg->FfxDx12Path.set_volatile_value((folder / kDll).wstring());

    if (!cfg->FfxDx12SRPath.has_value() && fs::exists(folder / kDllSR, ec))
        cfg->FfxDx12SRPath.set_volatile_value((folder / kDllSR).wstring());

    if (!cfg->FfxDx12FGPath.has_value() && fs::exists(folder / kFG, ec))
        cfg->FfxDx12FGPath.set_volatile_value((folder / kFG).wstring());

    if (!cfg->Dx12Upscaler.has_value())
        cfg->Dx12Upscaler.set_volatile_value("fsr31");
}

bool Found() { return found; }

const fs::path& Folder() { return folder; }

const char* Version() { return HELIXSR_SCALER_VERSION_STR; }

void Choose() { ChooseHelixSR(); }

bool IsActive()
{
    auto& state = State::Instance();
    auto feature = state.currentFeature;
    if (feature == nullptr || !feature->IsInited())
        return false;

    const auto code = BackendCode();
    if (code != "fsr31" && code != "fsr31_12")
        return false;

    const auto& names = state.ffxUpscalerVersionNames;
    const int index = Config::Instance()->FfxUpscalerIndex.value_or_default();
    return index >= 0 && index < (int) names.size() && names[index] != nullptr &&
           std::strstr(names[index], "HelixSR") != nullptr;
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

    changed |= Choice("Sharpness", &settings.sharpness, kSharpness, kSharpnessCount, labelWidth);
    Help(kSharpnessHelp);

    changed |= Choice("Speed boost", &settings.int8, kInt8, kInt8Count, labelWidth);
    Help(kInt8Help);

    if (!status.int8.empty())
    {
        ImGui::SetCursorPosX(labelWidth);
        ImGui::TextDisabled("Right now: %s", status.int8 == "on" ? "on" : "off");
    }

    changed |= Choice("GPU mode", &settings.wave, kWave, kWaveCount, labelWidth);
    Help(kWaveHelp);

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
