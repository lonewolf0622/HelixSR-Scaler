#include "pch.h"
#include "HelixSRNeuralRendering.h"
#include "../../helixsr/nr/runtime/HelixSrNrApi.h"
#include "../../helixsr/nr/runtime/TextureFormats.h"
#include <Util.h>
#include <Logger.h>
#include <imgui/imgui.h>
#include <hooks/D3D12_Hooks.h>
#include <detours/detours.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <cmath>
#include <format>

namespace HelixSR::Nr
{
namespace
{
using Microsoft::WRL::ComPtr;
std::mutex statusMutex;
std::string statusText = "Off";
void SetStatus(std::string text)
{
    std::lock_guard lock(statusMutex);
    statusText = std::move(text);
}
template <class T> T* Real(T* object)
{
    T* real = nullptr;
    return object && Util::CheckForRealObject(__FUNCTION__, object, (IUnknown**) &real) ? real : object;
}
void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    commands->ResourceBarrier(1, &barrier);
}
struct Library
{
    HMODULE module = nullptr;
    decltype(&HelixSR_NR_Create) create = nullptr;
    decltype(&HelixSR_NR_Record) record = nullptr;
    decltype(&HelixSR_NR_PrepareSubmission) prepare = nullptr;
    decltype(&HelixSR_NR_Submitted) submitted = nullptr;
    decltype(&HelixSR_NR_RetireRecording) retire = nullptr;
    decltype(&HelixSR_NR_Abort) abort = nullptr;
    decltype(&HelixSR_NR_Destroy) destroy = nullptr;
    decltype(&HelixSR_NR_LastError) error = nullptr;
    explicit Library(const std::filesystem::path& path)
    {
        module =
            LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module)
            throw std::runtime_error("Neural rendering files are not installed");
        try
        {
#define NR_LOAD(field, name)                                                                                           \
    field = reinterpret_cast<decltype(field)>(GetProcAddress(module, "HelixSR_NR_" name));                             \
    if (!field)                                                                                                        \
    throw std::runtime_error("Neural rendering backend version does not match Scaler")
            NR_LOAD(create, "Create");
            NR_LOAD(record, "Record");
            NR_LOAD(prepare, "PrepareSubmission");
            NR_LOAD(submitted, "Submitted");
            NR_LOAD(retire, "RetireRecording");
            NR_LOAD(abort, "Abort");
            NR_LOAD(destroy, "Destroy");
            NR_LOAD(error, "LastError");
#undef NR_LOAD
        }
        catch (...)
        {
            FreeLibrary(module);
            module = nullptr;
            throw;
        }
    }
    ~Library()
    {
        if (module)
            FreeLibrary(module);
    }
};
struct Owner
{
    std::shared_ptr<Library> library;
    ComPtr<ID3D12Device> device;
    HelixSrNrContext* context = nullptr;
    std::atomic_bool pending { false }, failed { false };
    ~Owner()
    {
        if (context && FAILED(library->destroy(context)))
        {
            // Registry ownership normally ends only after recordings close and
            // their actual queue fences finish. Preserve the backend on an
            // exceptional unresolved retirement rather than unloading live code.
            LOG_ERROR("NR: unresolved backend retirement: {}", library->error());
            auto* retained = new std::shared_ptr<Library>(library);
            (void) retained;
        }
    }
};
struct Use
{
    std::shared_ptr<Owner> owner;
    ComPtr<ID3D12Resource> scratch;
    uint64_t ticket = 0;
};
struct Timeline
{
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    uint64_t value = 0;
};
struct Recording
{
    ID3D12CommandList* identity = nullptr; // identity only; never dereferenced
    std::atomic_bool closed { false };
    bool executed = false, failed = false;
    std::vector<std::shared_ptr<Use>> uses;
    std::map<std::shared_ptr<Timeline>, uint64_t> completions;
};
struct Watch final : IUnknown
{
    std::atomic<ULONG> references { 1 };
    std::weak_ptr<Recording> recording;
    explicit Watch(const std::shared_ptr<Recording>& value) : recording(value) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown))
            return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto count = --references;
        if (!count)
        {
            if (auto value = recording.lock())
                value->closed = true;
            delete this;
        }
        return count;
    }
};
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
struct HookSlot
{
    ExecuteFn target = nullptr, original = nullptr;
};
struct Registry
{
    std::recursive_mutex mutex;
    std::mutex hooksMutex;
    std::array<HookSlot, 4> hooks {};
    GUID watchKey {};
    bool guidValid = SUCCEEDED(CoCreateGuid(&watchKey));
    std::vector<std::shared_ptr<Recording>> recordings;
    std::map<ID3D12CommandList*, std::shared_ptr<Recording>> active;
    std::vector<std::shared_ptr<Timeline>> timelines;
    std::atomic_bool hasRecords { false };
};
Registry& Records()
{
    static auto* value = new Registry;
    return *value;
}

bool DeviceGone(const Recording& record)
{
    return std::all_of(record.uses.begin(), record.uses.end(),
                       [](const auto& use) { return FAILED(use->owner->device->GetDeviceRemovedReason()); });
}
void CollectLocked(Registry& registry)
{
    std::erase_if(registry.recordings,
                  [&](const auto& record)
                  {
                      if (!record->closed)
                          return false;
                      const bool gone = DeviceGone(*record);
                      if (!gone && (record->failed ||
                                    !std::all_of(record->completions.begin(), record->completions.end(),
                                                 [](const auto& completion)
                                                 {
                                                     const auto done = completion.first->fence->GetCompletedValue();
                                                     return done != UINT64_MAX && done >= completion.second;
                                                 })))
                          return false;
                      for (auto& use : record->uses)
                      {
                          if (use->ticket && !gone)
                          {
                              const auto result = record->executed
                                                      ? use->owner->library->retire(use->owner->context, use->ticket)
                                                      : use->owner->library->abort(use->owner->context, use->ticket);
                              if (FAILED(result))
                              {
                                  record->failed = true;
                                  use->owner->failed = true;
                                  return false;
                              }
                              use->ticket = 0;
                          }
                          if (!record->executed)
                              use->owner->pending = false;
                      }
                      auto found = registry.active.find(record->identity);
                      if (found != registry.active.end() && found->second == record)
                          registry.active.erase(found);
                      return true;
                  });
    registry.hasRecords = !registry.recordings.empty();
}
std::shared_ptr<Use> Track(const std::shared_ptr<Owner>& owner, ID3D12GraphicsCommandList* commands,
                           ID3D12Resource* scratch)
{
    auto& registry = Records();
    std::lock_guard lock(registry.mutex);
    auto& active = registry.active[commands];
    if (!active || active->closed)
    {
        auto record = std::make_shared<Recording>();
        record->identity = commands;
        if (!registry.guidValid)
            throw std::runtime_error("Cannot track NR command-list lifetime");
        auto* watch = new Watch(record);
        const auto result = commands->SetPrivateDataInterface(registry.watchKey, watch);
        watch->Release();
        if (FAILED(result))
            throw std::runtime_error("Cannot watch NR command-list lifetime");
        registry.recordings.push_back(record);
        active = std::move(record);
    }
    auto use = std::make_shared<Use>();
    use->owner = owner;
    use->scratch = scratch;
    active->uses.push_back(use);
    registry.hasRecords = true;
    return use;
}
std::shared_ptr<Timeline> QueueTimeline(Registry& registry, ID3D12CommandQueue* queue)
{
    for (const auto& timeline : registry.timelines)
        if (timeline->queue.Get() == queue)
            return timeline;
    auto timeline = std::make_shared<Timeline>();
    timeline->queue = queue;
    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&timeline->fence))))
        throw std::runtime_error("Cannot create NR submission fence");
    registry.timelines.push_back(timeline);
    return timeline;
}
thread_local bool insideExecute = false;
template <size_t Slot>
void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    auto& registry = Records();
    const auto original = registry.hooks[Slot].original;
    if (insideExecute || !registry.hasRecords)
    {
        original(queue, count, lists);
        return;
    }
    std::lock_guard lock(registry.mutex);
    struct Scope
    {
        Scope() { insideExecute = true; }
        ~Scope() { insideExecute = false; }
    } scope;
    std::vector<std::shared_ptr<Recording>> matched;
    try
    {
        for (UINT i = 0; i < count; ++i)
        {
            auto found = registry.active.find(lists[i]);
            if (found != registry.active.end() && !found->second->closed &&
                std::find(matched.begin(), matched.end(), found->second) == matched.end())
                matched.push_back(found->second);
        }
        for (const auto& record : matched)
            for (const auto& use : record->uses)
                if (use->ticket && FAILED(use->owner->library->prepare(use->owner->context, use->ticket, queue)))
                {
                    record->failed = true;
                    use->owner->failed = true;
                }
    }
    catch (...)
    {
        for (const auto& record : registry.recordings)
        {
            record->failed = true;
            for (const auto& use : record->uses)
                use->owner->failed = true;
        }
    }
    original(queue, count, lists);
    try
    {
        if (!matched.empty())
        {
            for (const auto& record : matched)
            {
                record->executed = true;
                for (const auto& use : record->uses)
                    if (use->ticket)
                    {
                        if (FAILED(use->owner->library->submitted(use->owner->context, use->ticket, queue)))
                        {
                            record->failed = true;
                            use->owner->failed = true;
                        }
                        else
                            use->owner->pending = false;
                    }
            }
            auto timeline = QueueTimeline(registry, queue);
            const bool signaled =
                timeline->value < UINT64_MAX - 1 && SUCCEEDED(queue->Signal(timeline->fence.Get(), ++timeline->value));
            for (const auto& record : matched)
            {
                if (signaled)
                    record->completions[timeline] = timeline->value;
                else
                    record->failed = true;
            }
        }
        CollectLocked(registry);
    }
    catch (...)
    {
        for (const auto& record : matched)
            record->failed = true;
    }
}
bool EnsureHooks(ID3D12Device* device)
{
    auto& registry = Records();
    std::lock_guard lock(registry.hooksMutex);
    const std::array<ExecuteFn, 4> replacements { Execute<0>, Execute<1>, Execute<2>, Execute<3> };
    for (auto type : { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE })
    {
        D3D12_COMMAND_QUEUE_DESC desc {};
        desc.Type = type;
        ComPtr<ID3D12CommandQueue> queue;
        if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))))
            return false;
        auto* real = Real(queue.Get());
        const auto target = reinterpret_cast<ExecuteFn>((*reinterpret_cast<void***>(real))[10]);
        if (std::any_of(registry.hooks.begin(), registry.hooks.end(),
                        [&](const auto& hook) { return hook.target == target; }))
            continue;
        auto slot =
            std::find_if(registry.hooks.begin(), registry.hooks.end(), [](const auto& hook) { return !hook.target; });
        if (slot == registry.hooks.end())
            return false;
        const auto index = size_t(slot - registry.hooks.begin());
        slot->original = target;
        if (DetourTransactionBegin() != NO_ERROR)
        {
            slot->original = nullptr;
            return false;
        }
        DetourUpdateThread(GetCurrentThread());
        if (DetourAttach(reinterpret_cast<PVOID*>(&slot->original), reinterpret_cast<PVOID>(replacements[index])) !=
            NO_ERROR)
        {
            DetourTransactionAbort();
            slot->original = nullptr;
            return false;
        }
        if (DetourTransactionCommit() != NO_ERROR)
        {
            slot->original = nullptr;
            return false;
        }
        slot->target = target;
    }
    // Engine-owned private-data watches and the loader worker must remain valid
    // until process exit, including after the feature is switched off.
    HMODULE pin = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                              reinterpret_cast<LPCWSTR>(&EnsureHooks), &pin) != FALSE;
}
struct Key
{
    ComPtr<ID3D12Device> device;
    uint32_t width = 0, height = 0, networkWidth = 0, networkHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::filesystem::path cache;
    bool operator==(const Key& b) const
    {
        return device.Get() == b.device.Get() && width == b.width && height == b.height &&
               networkWidth == b.networkWidth && networkHeight == b.networkHeight && format == b.format &&
               cache == b.cache;
    }
};
struct Job
{
    Key key;
    std::shared_ptr<Owner> owner;
    std::string error;
    std::atomic_bool done { false }, canceled { false };
    std::atomic_uint completed { 0 }, total { 0 };
};
struct Loader
{
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Job>> jobs;
    Loader()
    {
        std::thread(
            [this]
            {
                for (;;)
                {
                    std::shared_ptr<Job> job;
                    {
                        std::unique_lock lock(mutex);
                        changed.wait(lock, [&] { return !jobs.empty(); });
                        job = jobs.front();
                        jobs.pop_front();
                    }
                    if (job->canceled)
                        continue;
                    try
                    {
                        auto owner = std::make_shared<Owner>();
                        owner->device = job->key.device;
                        owner->library = std::make_shared<Library>(Util::DllPath().parent_path() / L"HelixSR-NR" /
                                                                   L"helixsr-nr.dll");
                        const auto cache = job->key.cache.wstring();
                        HelixSrNrCreateInfo info {};
                        info.size = sizeof(info);
                        info.apiVersion = 2;
                        info.device = owner->device.Get();
                        info.cacheDirectory = cache.c_str();
                        info.planFile = L"runtime-plan.bin";
                        info.dataFile = L"initial-data.bin";
                        info.displayWidth = job->key.width;
                        info.displayHeight = job->key.height;
                        info.networkWidth = job->key.networkWidth;
                        info.networkHeight = job->key.networkHeight;
                        info.maximumFramesInFlight = 16;
                        info.progressUser = job.get();
                        info.progress = [](void* data, uint32_t done, uint32_t total)
                        {
                            auto* progress = static_cast<Job*>(data);
                            progress->total = total;
                            progress->completed = done;
                        };
                        if (FAILED(owner->library->create(&info, &owner->context)))
                            throw std::runtime_error(owner->library->error());
                        job->owner = std::move(owner);
                    }
                    catch (const std::exception& e)
                    {
                        job->error = e.what();
                        spdlog::warn("NR: preparation failed: {}", job->error);
                    }
                    job->done = true;
                }
            })
            .detach();
    }
    void submit(const std::shared_ptr<Job>& job)
    {
        {
            std::lock_guard lock(mutex);
            std::erase_if(jobs, [](const auto& pending) { return pending->canceled.load(); });
            jobs.push_back(job);
        }
        changed.notify_one();
    }
};
Loader& Worker()
{
    static auto* loader = new Loader;
    return *loader;
}
} // namespace

void ResetRecording(ID3D12GraphicsCommandList* commands)
{
    auto& registry = Records();
    if (!registry.hasRecords)
        return;
    std::lock_guard lock(registry.mutex);
    auto found = registry.active.find(Real(commands));
    if (found != registry.active.end())
    {
        found->second->closed = true;
        registry.active.erase(found);
    }
}
void Collect()
{
    auto& registry = Records();
    if (!registry.hasRecords)
        return;
    std::lock_guard lock(registry.mutex);
    CollectLocked(registry);
}
std::string Status()
{
    std::lock_guard lock(statusMutex);
    return statusText;
}

struct Pass::Impl
{
    std::shared_ptr<Job> job;
    std::shared_ptr<Owner> owner;
    ComPtr<ID3D12Resource> scratch;
    bool reset = true;
    bool loggedRecording = false;
    bool loggedOutput = false;
    std::string lastRecordError;
    uint32_t seed = 0;
    ~Impl()
    {
        if (job)
            job->canceled = true;
    }
};
Pass::Pass() : impl(std::make_unique<Impl>()) {}
Pass::~Pass() = default;
void Pass::Suspend()
{
    if (impl->job)
        impl->job->canceled = true;
    impl->job.reset();
    impl->owner.reset();
    impl->scratch.Reset();
    impl->reset = true;
    SetStatus("Off");
}
ID3D12Resource* Pass::Prepare(ID3D12Device* device, ID3D12GraphicsCommandList* commands, ID3D12Resource* output)
{
    if (!device || !commands || !output)
        return nullptr;
    Collect();
    auto& cfg = *Config::Instance();
    if (!cfg.HelixSRNREnabled.value_or_default())
    {
        Suspend();
        return nullptr;
    }
    if (cfg.HelixSRNRIntensity.value_or_default() <= 0.f)
    {
        // No neural result contributes at zero intensity. Keep the prepared
        // context, but reset history before the next frame that uses it.
        impl->reset = true;
        SetStatus("Neural Rendering: intensity 0 (bypassed)");
        return nullptr;
    }
    const auto desc = output->GetDesc();
    if (!impl->loggedOutput)
    {
        LOG_INFO("NR: requested output {}x{}, format {}, flags {}, mips {}, layers {}, samples {}", desc.Width,
                 desc.Height, unsigned(desc.Format), unsigned(desc.Flags), desc.MipLevels, desc.DepthOrArraySize,
                 desc.SampleDesc.Count);
        impl->loggedOutput = true;
    }
    if (helixnr::colorView(desc.Format) == DXGI_FORMAT_UNKNOWN ||
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
        desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1)
    {
        impl->reset = true;
        SetStatus("This output format is not supported yet");
        return nullptr;
    }
    auto* realDevice = Real(device);
    const auto edge = uint32_t(std::clamp(cfg.HelixSRNRNetworkSize.value_or_default(), 33, 4096));
    const auto longest = double(std::max(desc.Width, uint64_t(desc.Height)));
    Key key;
    key.device = realDevice;
    key.width = uint32_t(desc.Width);
    key.height = desc.Height;
    key.format = desc.Format;
    key.networkWidth = std::max(33u, uint32_t(std::round(edge * desc.Width / longest)));
    key.networkHeight = std::max(33u, uint32_t(std::round(edge * desc.Height / longest)));
    try
    {
        key.cache = cfg.HelixSRNRModelCache.has_value()
                        ? std::filesystem::path(string_to_wstring(cfg.HelixSRNRModelCache.value()))
                        : Util::DllPath().parent_path() / L"HelixSR-NR" / L"model-cache";
        if (key.cache.is_relative())
            key.cache = Util::DllPath().parent_path() / key.cache;
        if (!impl->job || !(impl->job->key == key))
        {
            if (!helixnr::supportsTextureView(realDevice, helixnr::colorView(desc.Format), false) ||
                !helixnr::supportsTextureView(realDevice, helixnr::colorView(desc.Format), true))
            {
                impl->reset = true;
                SetStatus("This device cannot write the output format for Neural Rendering");
                return nullptr;
            }
            if (impl->job)
                impl->job->canceled = true;
            impl->owner.reset();
            impl->scratch.Reset();
            impl->reset = true;
            impl->loggedRecording = false;
            impl->lastRecordError.clear();
            if (!EnsureHooks(realDevice))
            {
                SetStatus("Cannot track Neural Rendering submissions");
                return nullptr;
            }
            D3D12Hooks::HookToCommandListLate(commands);
            impl->job = std::make_shared<Job>();
            impl->job->key = key;
            Worker().submit(impl->job);
        }
        if (!impl->job->done)
        {
            SetStatus(
                std::format("Preparing Neural Rendering: {}/{}", impl->job->completed.load(), impl->job->total.load()));
            return nullptr;
        }
        if (!impl->job->error.empty())
        {
            SetStatus(impl->job->error);
            return nullptr;
        }
        impl->owner = impl->job->owner;
        if (impl->owner->failed)
        {
            SetStatus("Neural Rendering stopped after a submission error");
            return nullptr;
        }
        if (impl->owner->pending)
        {
            impl->reset = true;
            return nullptr;
        }
        if (!impl->scratch)
        {
            auto scratch = desc;
            scratch.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES heap {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            heap.CreationNodeMask = heap.VisibleNodeMask = 1;
            if (FAILED(realDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &scratch,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                           IID_PPV_ARGS(&impl->scratch))))
            {
                SetStatus("Not enough memory for Neural Rendering");
                return nullptr;
            }
        }
        SetStatus(std::format("Neural Rendering: {} x {}", key.networkWidth, key.networkHeight));
        return impl->scratch.Get();
    }
    catch (const std::exception& e)
    {
        SetStatus(e.what());
        return nullptr;
    }
}
bool Pass::Dispatch(ID3D12GraphicsCommandList* commands, ID3D12Resource* input, ID3D12Resource* output,
                    ID3D12Resource* depth, ID3D12Resource* motion, NVSDK_NGX_Parameter* parameters, bool hdr,
                    bool invertedDepth, bool lowResMotion, uint32_t renderWidth, uint32_t renderHeight)
{
    auto* list = Real(commands);
    auto* source = Real(input);
    auto* destination = Real(output);
    const auto fallback = [&]
    {
        Transition(list, source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(list, destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(destination, source);
        Transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(list, destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        impl->reset = true;
        return true;
    };
    auto use = Track(impl->owner, list, source);
    auto& cfg = *Config::Instance();
    HelixSrNrFrameInfo frame {};
    frame.size = sizeof(frame);
    frame.commands = list;
    frame.color = source;
    frame.output = destination;
    frame.colorState = frame.outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const auto desc = source->GetDesc();
    frame.colorRegion = { 0, 0, uint32_t(desc.Width), desc.Height };
    const auto guides = [&](ID3D12Resource* texture, HelixSrNrRegion& region, uint32_t width, uint32_t height,
                            const char* baseX, const char* baseY) -> ID3D12Resource*
    {
        auto* real = Real(texture);
        if (!real)
            return nullptr;
        const auto d = real->GetDesc();
        unsigned x = 0, y = 0;
        parameters->Get(baseX, &x);
        parameters->Get(baseY, &y);
        const auto availableWidth = x <= d.Width ? d.Width - x : 0;
        const auto availableHeight = y <= d.Height ? d.Height - y : 0;
        region = { x, y, uint32_t(std::min<uint64_t>(availableWidth, width)), std::min(availableHeight, height) };
        return real;
    };
    const auto motionWidth = lowResMotion ? renderWidth : uint32_t(desc.Width);
    const auto motionHeight = lowResMotion ? renderHeight : desc.Height;
    frame.depth =
        guides(depth, frame.depthRegion, renderWidth, renderHeight, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
               NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y);
    frame.motion =
        guides(motion, frame.motionRegion, motionWidth, motionHeight, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
               NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y);
    frame.depthState = frame.motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    float scaleX = 1, scaleY = 1;
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &scaleX);
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &scaleY);
    frame.motionScaleX = scaleX / std::max(1u, motionWidth);
    frame.motionScaleY = scaleY / std::max(1u, motionHeight);
    unsigned reset = 0;
    parameters->Get(NVSDK_NGX_Parameter_Reset, &reset);
    frame.reset = reset || impl->reset;
    frame.seed = impl->seed++;
    frame.autoMask = 1;
    frame.depthInverted = invertedDepth;
    frame.intensity = std::clamp(cfg.HelixSRNRIntensity.value_or_default(), 0.f, 1.f);
    frame.localTone = std::clamp(cfg.HelixSRNRLocalTone.value_or_default(), 0.f, 2.f);
    frame.localStructure = std::clamp(cfg.HelixSRNRLocalStructure.value_or_default(), 0.f, 2.f);
    frame.skinStructure = std::clamp(cfg.HelixSRNRSkinStructure.value_or_default(), -1.f, 2.f);
    frame.paperWhite = std::clamp(cfg.HelixSRNRPaperWhite.value_or_default(), .05f, 65504.f);
    frame.colorSpace = hdr ? 1u : 2u;
    frame.historyBlend = 1;
    frame.depthThreshold = .01f;
    const auto result = impl->owner->library->record(impl->owner->context, &frame, &use->ticket);
    if (FAILED(result))
    {
        const std::string error = impl->owner->library->error();
        if (impl->lastRecordError != error)
            LOG_WARN("NR: {}", error);
        impl->lastRecordError = error;
        SetStatus(error);
        return fallback();
    }
    if (!impl->loggedRecording)
    {
        LOG_INFO("NR: recording {}x{} output with {}x{} network", desc.Width, desc.Height, impl->job->key.networkWidth,
                 impl->job->key.networkHeight);
        impl->loggedRecording = true;
    }
    impl->lastRecordError.clear();
    impl->owner->pending = true;
    impl->reset = false;
    return true;
}
void RenderSettings()
{
    auto& cfg = *Config::Instance();
    bool enabled = cfg.HelixSRNREnabled.value_or_default();
    bool changed = ImGui::Checkbox("Neural rendering", &enabled);
    if (changed)
        cfg.HelixSRNREnabled = enabled;
    if (enabled)
    {
        float intensity = cfg.HelixSRNRIntensity.value_or_default();
        if (ImGui::SliderFloat("NR intensity", &intensity, 0, 1))
        {
            cfg.HelixSRNRIntensity = intensity;
            changed = true;
        }
        int size = cfg.HelixSRNRNetworkSize.value_or_default();
        if (ImGui::InputInt("NR resolution", &size, 32, 64))
        {
            cfg.HelixSRNRNetworkSize = std::clamp(size, 33, 4096);
            changed = true;
        }
        ImGui::TextDisabled("Higher resolution uses more GPU time.");
        ImGui::TextDisabled("Lower settings need a smaller prepared cache to save GPU time.");
    }
    ImGui::TextDisabled("%s", Status().c_str());
    if (changed)
        cfg.SaveIni();
}
} // namespace HelixSR::Nr
