#include "pch.h"

// HelixSR Scaler's HUD: the top of the menu window, drawn in the style of a game overlay (corner brackets, thin teal
// lines, monospace labels). It shows the upscaler choice, HelixSR's settings, live timings and HelixSR's state;
// OptiScaler's own settings follow under it in the same window, styled by ApplyHudStyle.

#include "HelixSR.h"
#include "HelixSRInternal.h"

#include "Config.h"
#include "State.h"
#include "resource.h"
#include "misc/IdentifyGpu.h"
#include "proxies/NVNGX_Proxy.h"
#include "upscalers/ffx/FFXFeature.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
using namespace HelixSR::detail;

// HelixSR's colours
constexpr unsigned kDark = 0x0b0e17;
constexpr unsigned kText = 0xe6e9f2;
constexpr unsigned kMuted = 0x8b93ab;
constexpr unsigned kTeal = 0x2dd4bf;
constexpr unsigned kViolet = 0xa78bfa;
constexpr unsigned kAmber = 0xf5b84b;
constexpr unsigned kRed = 0xf27a7a;

ImVec4 (*toColor)(const ImVec4&) = nullptr;
float k = 1.0f;   // pixels per design pixel (font size / 14)
float fs = 14.0f; // font size

ImU32 Col(unsigned hex, float a = 1.0f)
{
    ImVec4 c((float) ((hex >> 16) & 255) / 255.0f, (float) ((hex >> 8) & 255) / 255.0f, (float) (hex & 255) / 255.0f,
             a);
    return ImGui::ColorConvertFloat4ToU32(toColor != nullptr ? toColor(c) : c);
}

std::string Upper(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return (char) std::toupper(c); });
    return s;
}

ImVec2 TextSize(float size, const char* s) { return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, s); }

void Text(ImDrawList* dl, ImVec2 p, float size, ImU32 col, const char* s)
{
    dl->AddText(ImGui::GetFont(), size, p, col, s);
}

void Brackets(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float len, float th)
{
    dl->AddLine(a, ImVec2(a.x + len, a.y), col, th);
    dl->AddLine(a, ImVec2(a.x, a.y + len), col, th);
    dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x - len, a.y), col, th);
    dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x, a.y + len), col, th);
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(a.x + len, b.y), col, th);
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(a.x, b.y - len), col, th);
    dl->AddLine(b, ImVec2(b.x - len, b.y), col, th);
    dl->AddLine(b, ImVec2(b.x, b.y - len), col, th);
}

// A numbered panel: frame and title drawn now, contents placed by the caller inside [a, b]
void Panel(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* tag, const char* title)
{
    dl->AddRectFilled(a, b, Col(kDark, 0.72f));
    dl->AddRect(a, b, Col(kTeal, 0.18f));
    Brackets(dl, a, b, Col(kTeal), 10 * k, 2 * k);
    const float th = 22 * k;
    dl->AddRectFilled(a, ImVec2(a.x + 38 * k, a.y + th), Col(kTeal));
    const float ts = fs * 0.93f;
    Text(dl, ImVec2(a.x + 7 * k, a.y + (th - ts) * 0.5f), ts, Col(kDark), tag);
    Text(dl, ImVec2(a.x + 48 * k, a.y + (th - ts) * 0.5f), ts, Col(kTeal), title);
    const float lx = a.x + 48 * k + TextSize(ts, title).x + 10 * k;
    dl->AddLine(ImVec2(lx, a.y + th * 0.5f), ImVec2(b.x - 16 * k, a.y + th * 0.5f), Col(kTeal, 0.25f));
}

// [OPTION] OPTION OPTION on one line; returns true when the choice changed
bool Toggles(ImDrawList* dl, ImVec2 p, const char* id, const char* label, const char* help, const char* const* names,
             int n, int* value, bool enabled)
{
    const float ts = fs * 0.93f;
    Text(dl, p, ts, Col(kMuted, enabled ? 1.0f : 0.5f), label);

    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton((std::string(id) + "##label").c_str(), ImVec2(140 * k, ts + 4 * k));
    if (ImGui::IsItemHovered() && help != nullptr)
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(fs * 30.0f);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    bool changed = false;
    float x = p.x + 150 * k;
    for (int i = 0; i < n; i++)
    {
        const bool sel = *value == i;
        std::string s = Upper(names[i]);
        s = sel ? "[" + s + "]" : " " + s + " ";
        const ImVec2 sz = TextSize(ts, s.c_str());

        ImGui::SetCursorScreenPos(ImVec2(x, p.y - 2 * k));
        ImGui::BeginDisabled(!enabled);
        if (ImGui::InvisibleButton((std::string(id) + std::to_string(i)).c_str(), ImVec2(sz.x, sz.y + 6 * k)) && !sel)
        {
            *value = i;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::EndDisabled();

        const float a = enabled ? 1.0f : 0.4f;
        if (hovered && !sel)
            dl->AddRectFilled(ImVec2(x, p.y - 2 * k), ImVec2(x + sz.x, p.y + sz.y + 2 * k), Col(kTeal, 0.10f));
        Text(dl, ImVec2(x, p.y), ts, sel ? Col(kTeal, a) : Col(kText, (hovered ? 0.9f : 0.55f) * a), s.c_str());
        if (sel)
            dl->AddRectFilled(ImVec2(x, p.y + sz.y + 2 * k), ImVec2(x + sz.x, p.y + sz.y + 4 * k),
                              Col(kTeal, 0.7f * a));
        x += sz.x + 8 * k;
    }
    return changed;
}

// A text button in the HUD's style: [ LABEL ]
bool HudButton(ImDrawList* dl, ImVec2 p, const char* label)
{
    const float ts = fs * 0.93f;
    const ImVec2 sz = TextSize(ts, label);
    const ImVec2 size(sz.x + 20 * k, sz.y + 10 * k);
    ImGui::SetCursorScreenPos(p);
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), Col(kTeal, hovered ? 1.0f : 0.14f));
    dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), Col(kTeal));
    Text(dl, ImVec2(p.x + 10 * k, p.y + 5 * k), ts, hovered ? Col(kDark) : Col(kTeal), label);
    return pressed;
}

struct UpscalerEntry
{
    std::string name;
    ::Upscaler upscaler;
    bool helixsr;      // HelixSR: OptiScaler's DLSS upscaler with HelixSR's DLL
    int ffxIndex = -1; // FSR from the FFX SDK: its index in the SDK's version list (as OptiScaler's FFX Upscaler list)
};

// The upscalers of a DirectX 12 game, or of a DirectX 11 game (HelixSR, FSR from the FFX SDK and XeSS through
// OptiScaler's Dx12 interop there), or of a Vulkan game with HelixSR's FidelityFX DLL (HelixSR and FSR from the FFX SDK
// through the Dx12 interop)
std::vector<UpscalerEntry> Upscalers(API api)
{
    const bool dx11 = api == DX11, vulkan = api == Vulkan;
    std::vector<UpscalerEntry> list;
    const bool helixDlss = found && NVNGXProxy::HelixSRDlss() && !vulkan;
    if (helixDlss)
        list.push_back({ "HelixSR", HelixSRUpscaler(), true });

    // every FSR the FFX SDK reports (HelixSR's FidelityFX entry first, then FSR 4, FSR 3.1, ...), as OptiScaler's
    // own upscaler list names them
    const ::Upscaler ffx = dx11 || vulkan ? ::Upscaler::FFX_on12 : ::Upscaler::FFX;
    FFXFeature::EnsureVersionsDx12();
    const auto& versions = State::Instance().ffxUpscalerVersionsDx12;
    const auto order = HelixSR::Names::Order(versions.names);
    for (int n : order)
        list.push_back({ HelixSR::FfxVersionLabel(versions.names[n]), ffx, false, n });
    if (order.empty())
        list.push_back({ "FSR", ffx, false });

    list.push_back({ "XeSS", dx11 ? ::Upscaler::XeSS_on12 : ::Upscaler::XeSS, false });
    if (!helixDlss && IdentifyGpu::getPrimaryGpu().dlssCapable && Config::Instance()->DLSSEnabled.value_or_default())
        list.push_back({ "DLSS", ::Upscaler::DLSS, false });
    list.push_back({ "FSR 2.2.1", ::Upscaler::FSR22, false });
    list.push_back({ "FSR 2.1.2", dx11 || vulkan ? ::Upscaler::FSR21_on12 : ::Upscaler::FSR21, false });
    return list;
}

double Average(const std::deque<double>& values, size_t last)
{
    double sum = 0.0;
    size_t n = 0;
    for (auto it = values.rbegin(); it != values.rend() && n < last; ++it)
    {
        if (*it > 0.0)
        {
            sum += *it;
            n++;
        }
    }
    return n > 0 ? sum / (double) n : 0.0;
}

void Sparkline(ImDrawList* dl, ImVec2 a, ImVec2 b, const std::vector<double>& v)
{
    for (int i = 0; i <= 4; i++)
    {
        const float y = a.y + (b.y - a.y) * i / 4.0f;
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), Col(kTeal, 0.08f));
    }

    std::vector<double> data;
    for (double x : v)
        if (x > 0.0)
            data.push_back(x);
    if (data.size() < 2)
    {
        Text(dl, ImVec2(a.x + 4 * k, a.y + 4 * k), fs * 0.86f, Col(kMuted), "NO SAMPLES YET");
        return;
    }

    double hi = *std::ranges::max_element(data);
    hi = std::max(hi * 1.25, 0.5);
    std::vector<ImVec2> pts(data.size());
    for (size_t i = 0; i < data.size(); i++)
    {
        const float t = (float) i / (float) (data.size() - 1);
        pts[i] = ImVec2(a.x + t * (b.x - a.x), b.y - (float) (data[i] / hi) * (b.y - a.y));
    }
    dl->AddPolyline(pts.data(), (int) pts.size(), Col(kTeal), 0, 1.5f * k);
}

} // namespace

namespace HelixSR
{
// Only with HelixSR installed: otherwise the menu is OptiScaler's own
bool HudEnabled() { return Installed(); }

void ApplyHudStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.SeparatorTextBorderSize = 1.0f;

    // the colours stay OptiScaler's (Menu Theme and Colour), only lines take the accent
    ImVec4* c = style.Colors;
    const ImVec4 accent = c[ImGuiCol_CheckMark];
    c[ImGuiCol_Separator] = ImVec4(accent.x, accent.y, accent.z, 0.30f);
    c[ImGuiCol_Border] = ImVec4(accent.x, accent.y, accent.z, 0.25f);
}

void RenderHud(float scale, ImVec4 (*color)(const ImVec4&))
{
    (void) scale;
    toColor = color;
    RefreshMenu();

    auto& state = State::Instance();
    auto cfg = Config::Instance();
    auto& m = Menu();
    auto feature = state.currentFeature;
    const bool running = feature != nullptr && feature->IsInited();
    const bool active = IsActive();

    fs = ImGui::GetFontSize();
    k = fs / 14.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();

    // window: scanlines and corner brackets
    for (float y = wp.y; y < wp.y + ws.y; y += 3 * k)
        dl->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + ws.x, y), Col(0xffffff, 0.018f));
    Brackets(dl, wp, ImVec2(wp.x + ws.x - 1, wp.y + ws.y - 1), Col(kTeal), 22 * k, 3 * k);

    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float W = 980 * k;
    const float L = o.x, R = o.x + W;
    float y = o.y + 6 * k;

    // --- header ---
    const float big = fs * 1.85f, smallSize = fs * 0.86f, ts = fs * 0.93f;
    dl->AddRectFilled(ImVec2(L, y + 2 * k), ImVec2(L + 6 * k, y + big + 2 * k), Col(kTeal));
    Text(dl, ImVec2(L + 18 * k, y), big, Col(kText), "HELIXSR");
    Text(dl, ImVec2(L + 18 * k + TextSize(big, "HELIXSR ").x, y), big, Col(kTeal), "// SCALER");
    Text(dl, ImVec2(L + 20 * k, y + big + 4 * k), smallSize, Col(kMuted),
         "v" HELIXSR_SCALER_VERSION_STR "   OPTISCALER " OPTI_VERSION);

    std::string gpu = IdentifyGpu::getPrimaryGpu().name;
    if (gpu.empty())
        gpu = "--";
    const char* api = state.api == DX12 ? "D3D12" : state.api == DX11 ? "D3D11" : "VULKAN";
    const std::string input = running ? ApiUpscalerInputName(state.currentInputApiName) : "";
    const std::string apiLine = std::string("API   ") + api + "  /  INPUT " + Upper(input.empty() ? "--" : input) +
                                "  /  SPOOF " + (cfg->DxgiSpoofing.value_or_default() ? "ON" : "OFF");
    const float infoX = R - 440 * k;
    Text(dl, ImVec2(infoX, y + 2 * k), ts, Col(kMuted), ("GPU   " + Upper(gpu)).c_str());
    Text(dl, ImVec2(infoX, y + 20 * k), ts, Col(kMuted), apiLine.c_str());

    // state badge
    const char* badge;
    unsigned badgeCol;
    const bool fault = m.status.networkMissing || m.status.gpuUnsupported || m.status.networkFailed;
    if (!Installed())
        badge = "NO HELIXSR", badgeCol = kRed;
    else if (!running)
        badge = "STANDBY", badgeCol = kAmber;
    else if (active && fault)
        badge = "FAULT", badgeCol = kRed;
    else if (active)
        badge = "ONLINE", badgeCol = kTeal;
    else
        badge = "BYPASS", badgeCol = kViolet;
    const ImVec2 b0(R - 120 * k, y + 2 * k), b1(R, y + 32 * k);
    dl->AddRectFilled(b0, b1, Col(badgeCol, 0.14f));
    dl->AddRect(b0, b1, Col(badgeCol));
    dl->AddCircleFilled(ImVec2(b0.x + 15 * k, (b0.y + b1.y) * 0.5f), 4 * k, Col(badgeCol));
    Text(dl, ImVec2(b0.x + 27 * k, b0.y + (b1.y - b0.y - ts) * 0.5f), ts, Col(badgeCol), badge);

    y += 62 * k;
    dl->AddLine(ImVec2(L, y), ImVec2(R, y), Col(kTeal, 0.35f));
    for (float x = L; x < R; x += 40 * k)
        dl->AddLine(ImVec2(x, y - 3 * k), ImVec2(x, y + 3 * k), Col(kTeal, 0.35f));
    y += 20 * k;

    const float mid = L + W * 0.56f;
    const float row1 = 230 * k, row2 = 196 * k, gap = 18 * k;

    // --- 01 upscaler ---
    {
        const ImVec2 a(L, y), b(mid - 10 * k, y + row1);
        Panel(dl, a, b, "01", "UPSCALER");
        const float lx = a.x + 12 * k, ly = a.y + 36 * k;

        if (!running)
        {
            Text(dl, ImVec2(lx + 6 * k, ly), ts, Col(kText, 0.85f), "WAITING FOR THE GAME'S UPSCALER");
            Text(dl, ImVec2(lx + 6 * k, ly + 24 * k), ts, Col(kMuted), "TURN ON DLSS, FSR OR XESS IN THE");
            Text(dl, ImVec2(lx + 6 * k, ly + 42 * k), ts, Col(kMuted), "GAME'S SETTINGS AND LOAD A SAVE.");
        }
        else if (feature->GetUpscalerType() == ::Upscaler::DLSSD)
        {
            Text(dl, ImVec2(lx + 6 * k, ly), ts, Col(kText, 0.85f), "DLSS RAY RECONSTRUCTION IS ON");
            Text(dl, ImVec2(lx + 6 * k, ly + 24 * k), ts, Col(kMuted), "THE UPSCALER CAN'T BE CHANGED.");
        }
        else if (state.api != DX12 && state.api != DX11 && !FsrDllFound())
        {
            Text(dl, ImVec2(lx + 6 * k, ly), ts, Col(kText, 0.85f), "HELIXSR RUNS IN DIRECTX 11 AND 12 GAMES");
            Text(dl, ImVec2(lx + 6 * k, ly + 24 * k), ts, Col(kMuted), "PICK AN UPSCALER IN THE LIST BELOW.");
        }
        else
        {
            const auto list = Upscalers(state.api);
            const ::Upscaler current = Backend();
            const int ffxIndex = cfg->FfxUpscalerIndex.value_or_default();
            // rows shrink when the list is long (many FSR versions), so it stays inside the panel
            const float rowH = std::min(25 * k, (row1 - 44 * k) / (float) std::max<size_t>(list.size(), 1)),
                        rowW = 290 * k;
            for (size_t i = 0; i < list.size(); i++)
            {
                const auto& u = list[i];
                const bool isHelix = u.helixsr;
                const bool sameFfx = u.ffxIndex < 0 || u.ffxIndex == ffxIndex;
                // HelixSR's FidelityFX entry is an FFX entry: on when FFX runs it (by its index)
                const bool on = isHelix ? active : ((u.ffxIndex >= 0 || !active) && current == u.upscaler && sameFfx);
                const float ry = ly + i * rowH;

                ImGui::SetCursorScreenPos(ImVec2(lx, ry - 3 * k));
                ImGui::PushID(u.name.c_str());
                const bool clicked = ImGui::InvisibleButton("##up", ImVec2(rowW, rowH - 3 * k));
                const bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();

                if (on)
                    dl->AddRectFilled(ImVec2(lx, ry - 3 * k), ImVec2(lx + rowW, ry + rowH - 6 * k), Col(kTeal, 0.12f));
                else if (hovered)
                    dl->AddRectFilled(ImVec2(lx, ry - 3 * k), ImVec2(lx + rowW, ry + rowH - 6 * k), Col(kTeal, 0.06f));

                Text(dl, ImVec2(lx + 6 * k, ry), fs, on ? Col(kTeal) : Col(kText, 0.6f), on ? ">" : " ");
                Text(dl, ImVec2(lx + 26 * k, ry), fs, on || hovered ? Col(kText) : Col(kText, 0.6f),
                     Upper(u.name).c_str());
                if (on)
                    Text(dl, ImVec2(lx + 208 * k, ry + 2 * k), smallSize, Col(kTeal), "ACTIVE");
                else if (state.newBackend == u.upscaler && sameFfx && !on)
                    Text(dl, ImVec2(lx + 208 * k, ry + 2 * k), smallSize, Col(kAmber), "LOADING");

                if (clicked && !on)
                {
                    if (isHelix)
                        ChooseHelixSR();
                    else
                    {
                        if (u.ffxIndex >= 0)
                            cfg->FfxUpscalerIndex = u.ffxIndex; // which FSR the FFX SDK runs
                        ChooseBackend(u.upscaler);
                    }
                }
            }

            // render scale meter
            const float mx = a.x + 318 * k, my = a.y + 38 * k;
            const unsigned rh = feature->RenderHeight(), th = feature->TargetHeight();
            Text(dl, ImVec2(mx, my), smallSize, Col(kMuted), "RENDER  >  OUTPUT");
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%up", rh);
            Text(dl, ImVec2(mx, my + 20 * k), fs * 2.1f, Col(kText), buf);
            std::snprintf(buf, sizeof(buf), "%up", th);
            Text(dl, ImVec2(mx, my + 56 * k), fs * 2.1f, Col(kTeal), buf);
            const float ratio = th > 0 ? (float) rh / (float) th : 0.0f;
            const int filled = (int) std::lround(ratio * 20.0f);
            for (int i = 0; i < 20; i++)
            {
                const float x = mx + i * 11 * k;
                dl->AddRectFilled(ImVec2(x, my + 104 * k), ImVec2(x + 8 * k, my + 118 * k),
                                  i < filled ? Col(kTeal, 0.85f) : Col(kTeal, 0.15f));
            }
            std::snprintf(buf, sizeof(buf), "SCALE %.2fX", rh > 0 ? (float) th / (float) rh : 0.0f);
            Text(dl, ImVec2(mx, my + 126 * k), smallSize, Col(kMuted), buf);
        }
    }

    // --- 03 telemetry ---
    {
        const ImVec2 a(mid + 10 * k, y), b(R, y + row1);
        Panel(dl, a, b, "03", "TELEMETRY");

        std::vector<double> ups;
        double upAvg = 0.0, frameAvg = 0.0;
        {
            std::lock_guard<std::mutex> lock(state.frameTimeMutex);
            ups.assign(state.upscaleTimes.begin(), state.upscaleTimes.end());
            upAvg = Average(state.upscaleTimes, 30);
            frameAvg = Average(state.frameTimes, 30);
        }
        if (ups.size() > 90)
            ups.erase(ups.begin(), ups.end() - 90);
        Sparkline(dl, ImVec2(a.x + 16 * k, a.y + 36 * k), ImVec2(b.x - 16 * k, a.y + 112 * k), ups);

        char v[6][32];
        std::snprintf(v[0], 32, upAvg > 0 ? "%.2f MS" : "--", upAvg);
        std::snprintf(v[1], 32, frameAvg > 0 ? "%.1f MS" : "--", frameAvg);
        std::snprintf(v[3], 32, frameAvg > 0 ? "%.0f" : "--", frameAvg > 0 ? 1000.0 / frameAvg : 0.0);
        std::snprintf(v[2], 32, "%s", m.status.int8.empty() ? "--" : Upper(m.status.int8).c_str());
        if (m.status.networkWave > 0)
            std::snprintf(v[4], 32, "%d", m.status.networkWave);
        else
            std::snprintf(v[4], 32, "--");
        std::snprintf(v[5], 32, running ? "%ld" : "--", running ? feature->FrameCount() : 0L);
        // left column HelixSR, right column the game
        const char* keys[6] = { active ? "HELIXSR GPU" : "UPSCALE GPU", "FRAME", "INT8", "FPS", "WAVE", "FRAMES" };
        for (int i = 0; i < 6; i++)
        {
            const float x = a.x + 16 * k + (i % 2) * 200 * k, yy = a.y + 132 * k + (i / 2) * 28 * k;
            Text(dl, ImVec2(x, yy), smallSize, Col(kMuted), keys[i]);
            Text(dl, ImVec2(x + 96 * k, yy), ts, Col(i == 0 ? kAmber : kText), v[i]);
        }
    }

    y += row1 + gap;

    // --- 02 helixsr config ---
    {
        const ImVec2 a(L, y), b(mid - 10 * k, y + row2);
        Panel(dl, a, b, "02", "HELIXSR CONFIG");
        const float lx = a.x + 18 * k;
        bool changed = false;
        changed |= Toggles(dl, ImVec2(lx, a.y + 44 * k), "int8", "SPEED BOOST", kInt8Help, kInt8, kInt8Count,
                           &m.settings.int8, Installed());
        changed |= Toggles(dl, ImVec2(lx, a.y + 80 * k), "wave", "GPU MODE", kWaveHelp, kWave, kWaveCount,
                           &m.settings.wave, Installed());
        if (FrameGenInstalled())
            changed |= Toggles(dl, ImVec2(lx, a.y + 116 * k), "fg", "FRAME GEN", kFrameGenHelp, kFrameGen,
                               kFrameGenCount, &m.settings.frameGen, Installed());
        else
        {
            Text(dl, ImVec2(lx, a.y + 116 * k), fs * 0.93f, Col(kMuted, 0.5f), "FRAME GEN");
            Text(dl, ImVec2(lx + 150 * k, a.y + 116 * k), fs * 0.93f, Col(kMuted), "NOT INSTALLED");
        }
        if (changed)
            SaveMenuSettings();

        const ImVec2 note(lx, a.y + 154 * k);
        if (!Installed())
            Text(dl, note, smallSize, Col(kRed), "! NO HELIXSR FOLDER NEXT TO THE GAME");
        else if (m.saveFailed)
            Text(dl, note, smallSize, Col(kRed), "! COULDN'T SAVE THE SETTINGS");
        else if (m.restartNeeded)
            Text(dl, note, smallSize, Col(kAmber), "! SAVED. RESTART THE GAME TO APPLY");
        else
            Text(dl, note, smallSize, Col(kMuted), "HOVER A SETTING FOR HELP");
    }

    // --- 04 status ---
    {
        const ImVec2 a(mid + 10 * k, y), b(R, y + row2);
        Panel(dl, a, b, "04", "STATUS");
        const float lx = a.x + 18 * k;

        const char* network = "--";
        unsigned networkCol = kMuted;
        if (m.status.networkMissing)
            network = "NOT BUILT INTO THE DLL", networkCol = kRed;
        else if (m.status.gpuUnsupported)
            network = "GPU LACKS SHADER MODEL 6.6", networkCol = kRed;
        else if (m.status.networkFailed)
            network = "FAILED TO START", networkCol = kRed;
        else if (!m.status.version.empty())
            network = "RUNNING", networkCol = kTeal;

        struct Line
        {
            const char* key;
            std::string value;
            unsigned col;
        } lines[] = {
            { "HELIXSR", m.status.version.empty() ? "--" : m.status.version, kText },
            { "NETWORK", network, networkCol },
            { "FOLDER",
              found           ? "FOUND"
              : FsrDllFound() ? "HELIXSR-FSRFG"
                              : "MISSING",
              Installed() ? kTeal : kRed },
            { "LOG", "HELIXSR.LOG", kText },
        };
        for (int i = 0; i < 4; i++)
        {
            const float yy = a.y + 40 * k + i * 26 * k;
            Text(dl, ImVec2(lx, yy), smallSize, Col(kMuted), lines[i].key);
            Text(dl, ImVec2(lx + 100 * k, yy), ts, Col(lines[i].col), lines[i].value.c_str());
        }

        if (Installed() && HudButton(dl, ImVec2(lx, b.y - 40 * k), "OPEN HELIXSR FOLDER"))
            OpenFolder();
    }

    y += row2 + gap;

    // --- OptiScaler's settings follow ---
    const char* rule = "05 // OPTISCALER";
    Text(dl, ImVec2(L, y), ts, Col(kTeal), rule);
    const float rx = L + TextSize(ts, rule).x + 10 * k;
    dl->AddLine(ImVec2(rx, y + ts * 0.5f), ImVec2(R, y + ts * 0.5f), Col(kTeal, 0.25f));
    y += ts + 10 * k;

    // reserve the HUD's space so the window sizes itself around it
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(W, y - o.y));
}
} // namespace HelixSR
