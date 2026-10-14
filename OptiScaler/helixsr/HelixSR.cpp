#include "pch.h"

#include "HelixSR.h"

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

namespace
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

const char* kSharpness[] = { "Default", "Off", "Low", "Medium", "High" };
const float kSharpnessValue[] = { 0.0f, 0.0f, 0.1f, 0.3f, 0.5f };
const char* kInt8[] = { "Auto", "On", "Off" };
const char* kWave[] = { "Auto", "64", "32" };

struct Settings
{
    int sharpness = 0;
    int int8 = 0;
    int wave = 0;
    std::string sharpener; // kept when it is one HelixSR knows
};

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

struct LogStatus
{
    std::string version; // "1.9.0"
    std::string int8;    // "on" / "off"
    int networkWave = 0; // 32 / 64
    bool networkMissing = false;
    bool gpuUnsupported = false;
    bool networkFailed = false;
};

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

void UseHelixSR()
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

ImVec4 Green() { return toColor(ImVec4(0.176f, 0.831f, 0.749f, 1.0f)); }  // HelixSR teal
ImVec4 Yellow() { return toColor(ImVec4(0.961f, 0.722f, 0.294f, 1.0f)); } // HelixSR amber
ImVec4 Red() { return toColor(ImVec4(0.949f, 0.478f, 0.478f, 1.0f)); }    // HelixSR danger

void Dim(const char* text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void Headline(const ImVec4& color, const char* text, float scale)
{
    if (Config::Instance()->UseHQFont.value_or_default())
    {
        ImGui::PushFontSize(std::round(ImGui::GetFontSize() * 1.4f));
        ImGui::TextColored(color, "%s", text);
        ImGui::PopFontSize();
    }
    else
    {
        ImGui::SetWindowFontScale(scale * 1.4f);
        ImGui::TextColored(color, "%s", text);
        ImGui::SetWindowFontScale(scale);
    }
}

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

// The upscaler's state in plain words
void RenderStatus(const LogStatus& log, float scale)
{
    auto& state = State::Instance();
    auto feature = state.currentFeature;

    if (!found)
    {
        Headline(Red(), "HelixSR isn't installed here", scale);
        ImGui::TextWrapped("Put the HelixSR folder next to this file, in the game's folder, and start the game "
                           "again. The HelixSR app does this for you.");
        return;
    }

    if (log.gpuUnsupported)
    {
        Headline(Red(), "Your graphics card can't run HelixSR", scale);
        ImGui::TextWrapped("Update your graphics driver. Cards older than AMD Vega / RDNA 1 aren't supported.");
        return;
    }

    if (log.networkMissing || log.networkFailed)
    {
        Headline(Red(), "HelixSR isn't set up yet", scale);
        ImGui::TextWrapped("Run the HelixSR setup (or the HelixSR app) and install HelixSR into this game again. "
                           "Until then you see a red frame around a blurry picture.");
        return;
    }

    if (feature == nullptr || !feature->IsInited())
    {
        Headline(Yellow(), "Waiting for the game", scale);
        ImGui::TextWrapped("In the game's graphics settings, turn on DLSS (or FSR or XeSS if there is no DLSS), "
                           "then load into the game.");
        return;
    }

    if (feature->IsFrozen())
    {
        Headline(Yellow(), "Paused", scale);
        ImGui::TextWrapped("The game isn't upscaling right now (for example in menus). Load into the game.");
        return;
    }

    if (!HelixSR::IsActive())
    {
        Headline(Yellow(), "HelixSR is off", scale);
        ImGui::TextWrapped("The game is using %s instead.", feature->Name().c_str());

        if (state.api == Vulkan || state.api == DX11)
            Dim("This game doesn't use DirectX 12. HelixSR runs through DirectX 12 here, which may not work in "
                "every game.");

        if (ImGui::Button("Use HelixSR"))
            UseHelixSR();
        return;
    }

    Headline(Green(), "HelixSR is running", scale);

    const auto renderH = feature->RenderHeight();
    const auto targetH = feature->TargetHeight();
    if (renderH > 0 && targetH > 0)
    {
        if (renderH == targetH)
            ImGui::Text("%up, no upscaling (best picture)", targetH);
        else
            ImGui::Text("%up upscaled to %up (%.2fx)", renderH, targetH, (float) targetH / (float) renderH);
    }
}
} // namespace

namespace HelixSR
{
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

void RenderSection(float scale, ImVec4 (*color)(const ImVec4&))
{
    toColor = color;

    // the files are read when the section is drawn and then every two seconds
    static LogStatus status;
    static Settings settings;
    static Settings saved;
    static double lastRead = -10.0;
    static bool restartNeeded = false;
    static bool saveFailed = false;

    const double now = ImGui::GetTime();
    if (found && now - lastRead > 2.0)
    {
        status = ReadLog();
        if (lastRead < 0.0)
            settings = saved = ReadSettings();
        lastRead = now;
    }

    ImGui::SeparatorText("HelixSR");
    ImGui::Spacing();

    RenderStatus(status, scale);

    if (!found)
        return;

    ImGui::Spacing();

    const float labelWidth = 130.0f * scale;
    bool changed = false;

    changed |= Choice("Sharpness", &settings.sharpness, kSharpness, (int) IM_ARRAYSIZE(kSharpness), labelWidth);
    Help("How crisp the picture looks. Default is what this HelixSR version uses. If edges sparkle or shimmer, try "
         "Off.");

    changed |= Choice("Speed boost", &settings.int8, kInt8, (int) IM_ARRAYSIZE(kInt8), labelWidth);
    Help("Faster math (int8) on graphics cards that support it. Auto turns it on where it helps. If the picture "
         "shimmers or flickers, try Off.");

    if (!status.int8.empty())
    {
        ImGui::SetCursorPosX(labelWidth);
        ImGui::TextDisabled("Right now: %s", status.int8 == "on" ? "on" : "off");
    }

    changed |= Choice("GPU mode", &settings.wave, kWave, (int) IM_ARRAYSIZE(kWave), labelWidth);
    Help("How many threads HelixSR runs together (wave size). Auto is right for most cards. For AMD RX 7000 / 9000 "
         "with a broken picture, try 64.");

    if (status.networkWave > 0)
    {
        ImGui::SetCursorPosX(labelWidth);
        ImGui::TextDisabled("Right now: %d", status.networkWave);
    }

    if (ImGui::Button("Open the HelixSR folder"))
        OpenFolder();
    Help("helixsr.log in this folder tells what HelixSR did. Attach it when you report a problem.");

    if (changed)
    {
        saveFailed = !WriteSettings(settings);
        if (!saveFailed)
            restartNeeded =
                settings.sharpness != saved.sharpness || settings.int8 != saved.int8 || settings.wave != saved.wave;
        saved = settings;
    }

    if (saveFailed)
        ImGui::TextColored(Red(), "Couldn't save helixsr.ini in the HelixSR folder.");
    else if (restartNeeded)
        ImGui::TextColored(Yellow(), "Saved. Restart the game to use the new settings.");

    ImGui::Spacing();
    ImGui::TextDisabled("HelixSR Scaler %s%s%s", Version(), status.version.empty() ? "" : " - HelixSR ",
                        status.version.c_str());
}
} // namespace HelixSR
