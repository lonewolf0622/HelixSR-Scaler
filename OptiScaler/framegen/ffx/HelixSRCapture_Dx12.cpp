#include "pch.h"
#include "HelixSRCapture_Dx12.h"

#include <Config.h>
#include <Util.h>

#include <sl.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

// HelixSR Scaler: see HelixSRCapture_Dx12.h

namespace
{
constexpr int kFrames = 4;                       // capture frames N .. N+3
constexpr int kTagBefore = 1, kTagAfter = 1;     // tags also of the frame before and after (pipelined games)
constexpr int kDumpDelay = 4;                    // presents after the fence signal before the files are written
constexpr uint64_t kCap = 2000ull * 1024 * 1024; // read-back bytes at most

struct Item
{
    ID3D12Resource* readback = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    std::string file;
};

struct Deferred
{
    ID3D12Resource* resource = nullptr; // AddRef'd
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    std::string name;
    std::string extra;
    int group = -1;
};

struct Configured
{
    ID3D12Resource* hudless = nullptr;
    D3D12_RESOURCE_STATES hudlessState = D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* ui = nullptr;
    D3D12_RESOURCE_STATES uiState = D3D12_RESOURCE_STATE_COMMON;
    int group = -1;
};

std::mutex g_m;
bool g_configRead = false;
int g_start = 0; // N, 0 = off
int g_frame = 0; // presents of the FSR FG output so far
bool g_done = false;
uint64_t g_bytes = 0;
int g_seq = 0;
std::vector<Item> g_items;
std::vector<std::string> g_lines;
std::vector<Deferred> g_deferred;
std::vector<ID3D12CommandAllocator*> g_allocators;
std::vector<ID3D12GraphicsCommandList*> g_lists;
std::map<uint64_t, Configured> g_configured;
ID3D12Fence* g_fence = nullptr;
int g_signalFrame = -1;

thread_local const char* t_source = "direct";
thread_local uint32_t t_viewport = 0;
thread_local int t_feature = -1;

bool EnabledLocked()
{
    if (!g_configRead)
    {
        g_configRead = true;
        auto value = Config::Instance()->FGHelixSRDebugCapture.value_or(0);
        g_start = value > 0 ? value : 0;

        if (g_start > 0)
            LOG_INFO("HelixSR capture: frames {} .. {} of the FSR FG output are captured", g_start,
                     g_start + kFrames - 1);
    }

    return g_start > 0 && !g_done;
}

bool InDispatchWindow(int group) { return group >= g_start && group < g_start + kFrames; }

bool InTagWindow(int group) { return group >= g_start - kTagBefore && group < g_start + kFrames + kTagAfter; }

std::string TypeName(uint32_t slType)
{
    switch (slType)
    {
    case sl::kBufferTypeDepth:
        return "depth";
    case sl::kBufferTypeMotionVectors:
        return "mv";
    case sl::kBufferTypeHUDLessColor:
        return "hudless";
    case sl::kBufferTypeUIColorAndAlpha:
        return "ui";
    case sl::kBufferTypeHiResDepth:
        return "hiresdepth";
    case sl::kBufferTypeLinearDepth:
        return "lineardepth";
    case sl::kBufferTypeBidirectionalDistortionField:
        return "distortion";
    case sl::kBufferTypeBackbuffer:
        return "backbuffer";
    default:
        return std::format("type{}", slType);
    }
}

void LineLocked(int group, const std::string& text) { g_lines.push_back(std::format("g {} {}", group, text)); }

void CopyLocked(int group, const char* point, const std::string& name, ID3D12GraphicsCommandList* list,
                ID3D12Resource* resource, D3D12_RESOURCE_STATES state, const std::string& extra)
{
    if (list == nullptr || resource == nullptr)
        return;

    const auto desc = resource->GetDesc();

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1)
    {
        LineLocked(group, std::format("skip {} {}: not a single-sample 2D texture", point, name));
        return;
    }

    // Copy and compute lists cannot use the graphics states
    const auto graphicsStates = D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ |
                                D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && (state & graphicsStates) != 0)
    {
        LineLocked(group, std::format("skip {} {}: state {:#x} on a list of type {}", point, name, (UINT) state,
                                      (int) list->GetType()));
        return;
    }

    ID3D12Device* device = nullptr;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return;

    Item item {};
    UINT64 total = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &item.footprint, &item.rows, &item.rowBytes, &total);

    if (total == 0 || total == UINT64_MAX || g_bytes + total > kCap)
    {
        LineLocked(group, std::format("skip {} {}: {} bytes (size cap, {} used)", point, name, total, g_bytes));
        device->Release();
        return;
    }

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = total;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    auto hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&item.readback));
    device->Release();

    if (FAILED(hr) || item.readback == nullptr)
    {
        LineLocked(group, std::format("skip {} {}: read-back buffer {:X}", point, name, (UINT) hr));
        return;
    }

    const bool transition = state != D3D12_RESOURCE_STATE_COPY_SOURCE;
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = state;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    if (transition)
        list->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = item.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = item.footprint;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    if (transition)
    {
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
    }

    g_bytes += total;
    item.file = std::format("g{}-{}-{}-{}.bin", group, point, name, g_seq++);

    LineLocked(group, std::format("file {} point {} name {} w {} h {} fmt {} footprintfmt {} rowbytes {} rows {} "
                                  "res {:X} state {:#x} {}",
                                  item.file, point, name, desc.Width, desc.Height, (UINT) desc.Format,
                                  (UINT) item.footprint.Footprint.Format, item.rowBytes, item.rows, (size_t) resource,
                                  (UINT) state, extra));

    g_items.push_back(item);
}

ID3D12GraphicsCommandList* NewListLocked(ID3D12Device* device)
{
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
        return nullptr;

    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list))))
    {
        allocator->Release();
        return nullptr;
    }

    g_allocators.push_back(allocator);
    g_lists.push_back(list);
    return list;
}

// The files, on a worker thread (the read-back buffers are complete: the fence passed)
void Dump(std::vector<Item> items, std::vector<std::string> lines, std::vector<ID3D12CommandAllocator*> allocators,
          std::vector<ID3D12GraphicsCommandList*> lists)
{
    std::filesystem::path dir;

    try
    {
        auto now = std::chrono::system_clock::now();
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        dir = Util::ExePath().parent_path() / "helixsr-capture" / std::format("{}", seconds);
        std::filesystem::create_directories(dir);
    }
    catch (...)
    {
        LOG_ERROR("HelixSR capture: the folder could not be made");
    }

    uint64_t written = 0;

    for (auto& item : items)
    {
        void* data = nullptr;
        D3D12_RANGE range { 0,
                            (SIZE_T) (item.footprint.Offset + item.footprint.Footprint.RowPitch * (UINT64) item.rows) };

        if (!dir.empty() && SUCCEEDED(item.readback->Map(0, &range, &data)) && data != nullptr)
        {
            std::ofstream file(dir / item.file, std::ios::binary);
            auto bytes = (const char*) data + item.footprint.Offset;

            for (UINT row = 0; row < item.rows && file; row++)
                file.write(bytes + (size_t) row * item.footprint.Footprint.RowPitch, (std::streamsize) item.rowBytes);

            written += item.rowBytes * item.rows;

            D3D12_RANGE none { 0, 0 };
            item.readback->Unmap(0, &none);
        }

        item.readback->Release();
    }

    if (!dir.empty())
    {
        std::ofstream text(dir / "capture.txt");
        text << "# HelixSR Scaler capture: 'g <group>' = frame (present of the FSR FG output); A = the game's inputs "
                "as received, B = what the frame-generation DLL is given\n";

        for (auto& line : lines)
            text << line << "\n";
    }

    for (auto list : lists)
        list->Release();

    for (auto allocator : allocators)
        allocator->Release();

    LOG_INFO("HelixSR capture: {} files, {} MB written to {}", items.size(), written >> 20, dir.string());
}
} // namespace

namespace HelixSRCapture
{
bool Enabled()
{
    std::scoped_lock lock(g_m);
    return EnabledLocked();
}

int DispatchGroup()
{
    std::scoped_lock lock(g_m);
    return (EnabledLocked() && InDispatchWindow(g_frame)) ? g_frame : -1;
}

int TagGroup()
{
    std::scoped_lock lock(g_m);
    return (EnabledLocked() && InTagWindow(g_frame + 1)) ? g_frame + 1 : -1;
}

Source::Source(const char* name, uint32_t viewport, int feature)
{
    t_source = name;
    t_viewport = viewport;
    t_feature = feature;
}

Source::~Source()
{
    t_source = "direct";
    t_viewport = 0;
    t_feature = -1;
}

std::string ResText(ID3D12Resource* resource)
{
    if (resource == nullptr)
        return "none";

    auto desc = resource->GetDesc();
    return std::format("{:X} {}x{} fmt {} flags {:#x}", (size_t) resource, desc.Width, desc.Height, (UINT) desc.Format,
                       (UINT) desc.Flags);
}

void OnPresent(ID3D12CommandQueue* queue, IDXGISwapChain* swapChain, bool generating)
{
    std::scoped_lock lock(g_m);

    if (!EnabledLocked() || queue == nullptr)
        return;

    // the window counts presents with frame generation running; once in it, every present counts
    if (generating || g_frame >= g_start - kTagBefore - 1)
        g_frame++;

    const int group = g_frame;

    // the back buffer of this present and the tags that waited for it
    if (InTagWindow(group) && (InDispatchWindow(group) || !g_deferred.empty()))
    {
        ID3D12Device* device = nullptr;

        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && device != nullptr)
        {
            if (auto list = NewListLocked(device); list != nullptr)
            {
                for (auto& d : g_deferred)
                {
                    CopyLocked(d.group, "A-present", d.name, list, d.resource, d.state, d.extra);
                    d.resource->Release();
                }

                g_deferred.clear();

                IDXGISwapChain3* sc3 = nullptr;

                if (InDispatchWindow(group) && swapChain != nullptr &&
                    SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3 != nullptr)
                {
                    ID3D12Resource* backbuffer = nullptr;
                    auto index = sc3->GetCurrentBackBufferIndex();

                    if (SUCCEEDED(sc3->GetBuffer(index, IID_PPV_ARGS(&backbuffer))) && backbuffer != nullptr)
                    {
                        CopyLocked(group, "A-present", "backbuffer", list, backbuffer, D3D12_RESOURCE_STATE_PRESENT,
                                   std::format("index {}", index));
                        backbuffer->Release();
                    }

                    sc3->Release();
                }

                if (SUCCEEDED(list->Close()))
                    queue->ExecuteCommandLists(1, (ID3D12CommandList**) &list);
            }

            if (g_fence == nullptr)
                device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));

            device->Release();
        }
    }

    // after the window (the generation of its last frame was submitted at that present): the fence, then the files
    if (g_signalFrame < 0 && group > g_start + kFrames + kTagAfter && g_fence != nullptr)
    {
        for (auto& d : g_deferred)
            d.resource->Release();

        g_deferred.clear();

        queue->Signal(g_fence, 1);
        g_signalFrame = group;
    }

    if (g_signalFrame >= 0 && group >= g_signalFrame + kDumpDelay && g_fence->GetCompletedValue() >= 1)
    {
        g_done = true;
        std::thread(Dump, std::move(g_items), std::move(g_lines), std::move(g_allocators), std::move(g_lists)).detach();
        g_items.clear();
        g_lines.clear();
        g_allocators.clear();
        g_lists.clear();
        g_configured.clear();
        g_fence->Release();
        g_fence = nullptr;
    }
}

void Tag(uint32_t slType, int lifecycle, ID3D12Resource* resource, uint32_t state, uint32_t left, uint32_t top,
         uint64_t width, uint32_t height, uint32_t slFrame, ID3D12GraphicsCommandList* cmdList, int slot, bool accepted)
{
    std::scoped_lock lock(g_m);

    if (!EnabledLocked() || !InTagWindow(g_frame + 1) || resource == nullptr)
        return;

    const int group = g_frame + 1;
    const auto name = TypeName(slType);
    const auto listType = cmdList != nullptr ? (int) cmdList->GetType() : -1;
    const auto extra = std::format("slframe {} slot {} lifecycle {}", slFrame, slot, lifecycle);

    LineLocked(group,
               std::format("tag src {} viewport {} feature {} type {} ({}) lifecycle {} res {} state {:#x} "
                           "extent {} {} {} {} slframe {} slot {} accepted {} cmdlist {:X} type {}",
                           t_source, t_viewport, t_feature, slType, name, lifecycle, ResText(resource), state, left,
                           top, width, height, slFrame, slot, accepted ? 1 : 0, (size_t) cmdList, listType));

    if (cmdList != nullptr && listType == (int) D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        CopyLocked(group, "A-tag", name, cmdList, resource, (D3D12_RESOURCE_STATES) state, extra);
    }
    else
    {
        resource->AddRef();
        g_deferred.push_back({ resource, (D3D12_RESOURCE_STATES) state, name, extra, group });
    }
}

void Constants(const sl::Constants& given, uint32_t slFrame, int slot, float nearOut, float farOut, float fovOut,
               float aspectOut, float mvScaleX, float mvScaleY, bool infinite)
{
    std::scoped_lock lock(g_m);

    if (!EnabledLocked() || !InTagWindow(g_frame + 1))
        return;

    const int group = g_frame + 1;

    auto matrix = [](const sl::float4x4& m)
    {
        std::string s;
        for (uint32_t r = 0; r < 4; r++)
            s += std::format(" {:.9g} {:.9g} {:.9g} {:.9g}", m[r].x, m[r].y, m[r].z, m[r].w);
        return s;
    };

    LineLocked(group,
               std::format("constants src {} viewport {} slframe {} slot {}", t_source, t_viewport, slFrame, slot));
    LineLocked(group, std::format("mat cameraViewToClip{}", matrix(given.cameraViewToClip)));
    LineLocked(group, std::format("mat clipToCameraView{}", matrix(given.clipToCameraView)));
    LineLocked(group, std::format("mat clipToLensClip{}", matrix(given.clipToLensClip)));
    LineLocked(group, std::format("mat clipToPrevClip{}", matrix(given.clipToPrevClip)));
    LineLocked(group, std::format("mat prevClipToClip{}", matrix(given.prevClipToClip)));
    LineLocked(group,
               std::format("given jitter {:.9g} {:.9g} mvecScale {:.9g} {:.9g} pinhole {:.9g} {:.9g} pos {:.9g} {:.9g} "
                           "{:.9g} up {:.9g} {:.9g} {:.9g} right {:.9g} {:.9g} {:.9g} fwd {:.9g} {:.9g} {:.9g}",
                           given.jitterOffset.x, given.jitterOffset.y, given.mvecScale.x, given.mvecScale.y,
                           given.cameraPinholeOffset.x, given.cameraPinholeOffset.y, given.cameraPos.x,
                           given.cameraPos.y, given.cameraPos.z, given.cameraUp.x, given.cameraUp.y, given.cameraUp.z,
                           given.cameraRight.x, given.cameraRight.y, given.cameraRight.z, given.cameraFwd.x,
                           given.cameraFwd.y, given.cameraFwd.z));
    LineLocked(group, std::format("given near {:.9g} far {:.9g} fov {:.9g} aspect {:.9g} mvInvalid {:.9g} "
                                  "depthInverted {} cameraMotionIncluded {} mv3D {} reset {} ortho {} mvDilated {} "
                                  "mvJittered {} minSeparation {:.9g}",
                                  given.cameraNear, given.cameraFar, given.cameraFOV, given.cameraAspectRatio,
                                  given.motionVectorsInvalidValue, (int) given.depthInverted,
                                  (int) given.cameraMotionIncluded, (int) given.motionVectors3D, (int) given.reset,
                                  (int) given.orthographicProjection, (int) given.motionVectorsDilated,
                                  (int) given.motionVectorsJittered, given.minRelativeLinearDepthObjectSeparation));
    LineLocked(group, std::format("made near {:.9g} far {:.9g} fovV {:.9g} aspect {:.9g} mvScale {:.9g} {:.9g} "
                                  "infinite {}",
                                  nearOut, farOut, fovOut, aspectOut, mvScaleX, mvScaleY, infinite ? 1 : 0));
}

void Copy(int group, const char* point, const char* name, ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
          D3D12_RESOURCE_STATES state, const std::string& extra)
{
    std::scoped_lock lock(g_m);

    if (!EnabledLocked() || group < 0)
        return;

    CopyLocked(group, point, name, list, resource, state, extra);
}

void Line(int group, const std::string& text)
{
    std::scoped_lock lock(g_m);

    if (!EnabledLocked() || group < 0)
        return;

    LineLocked(group, text);
}

void NoteConfigure(uint64_t frameID, ID3D12Resource* hudless, D3D12_RESOURCE_STATES hudlessState, ID3D12Resource* ui,
                   D3D12_RESOURCE_STATES uiState)
{
    std::scoped_lock lock(g_m);

    if (!EnabledLocked() || !InDispatchWindow(g_frame))
        return;

    g_configured[frameID] = { hudless, hudlessState, ui, uiState, g_frame };
}

bool ConfiguredFor(uint64_t frameID, ID3D12Resource** hudless, D3D12_RESOURCE_STATES* hudlessState, ID3D12Resource** ui,
                   D3D12_RESOURCE_STATES* uiState, int* group)
{
    std::scoped_lock lock(g_m);

    if (!EnabledLocked())
        return false;

    auto it = g_configured.find(frameID);
    if (it == g_configured.end())
        return false;

    *hudless = it->second.hudless;
    *hudlessState = it->second.hudlessState;
    *ui = it->second.ui;
    *uiState = it->second.uiState;
    *group = it->second.group;
    return true;
}
} // namespace HelixSRCapture
