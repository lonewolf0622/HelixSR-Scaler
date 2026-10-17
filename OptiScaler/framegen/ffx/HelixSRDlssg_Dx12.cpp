#include "pch.h"
#include "HelixSRDlssg_Dx12.h"

#include <State.h>

#include <sl.h>

#include <atomic>
#include <format>
#include <mutex>
#include <vector>

namespace HelixSRDlssg
{
namespace
{
enum Kind
{
    kDepth,
    kMotionVectors,
    kHudless,
    kUi,
    kKinds
};

constexpr int kConstantSlots = 8; // Streamline frames whose constants are kept
constexpr int kTagSlots = 4;      // Streamline frames whose tags (and copies) are kept
constexpr int kRetireFrames = 8;  // presents before a replaced copy is released (the GPU is done with it)

struct ConstantSlot
{
    uint32_t slFrame = 0;
    bool set = false;
    bool used = false; // given to a prepare already: not again
    sl::Constants constants {};
};

struct TagSlot
{
    uint32_t slFrame = 0;
    bool set[kKinds] {};
    HelixSRFfxDlssgResource res[kKinds] {};
};

struct Retired
{
    ID3D12Resource* resource = nullptr;
    int framesLeft = 0;
};

std::mutex dataMutex;
std::atomic<bool> wanted { false };
ConstantSlot constantsByFrame[kConstantSlots] {};
uint32_t slFrameAtIndex[BUFFER_COUNT] {}; // the Streamline frame OptiScaler's FG index holds (fallback pairing)
bool slFrameAtIndexSet[BUFFER_COUNT] {};
TagSlot tags[kTagSlots] {};
ID3D12Resource* copies[kTagSlots][kKinds] {};
std::vector<Retired> retired;
uint32_t dlssgViewport = 0;
bool dlssgViewportSet = false;
uint32_t presentFrame = 0;
bool presentSeen = false;

bool IsTrue(sl::Boolean b) { return b == sl::Boolean::eTrue; }

int KindOf(sl::BufferType type)
{
    // the types sl.dlss_g reads (kBufferTypeDepth only: HiResDepth / LinearDepth are other buffers)
    if (type == sl::kBufferTypeDepth)
        return kDepth;
    if (type == sl::kBufferTypeMotionVectors)
        return kMotionVectors;
    if (type == sl::kBufferTypeHUDLessColor)
        return kHudless;
    if (type == sl::kBufferTypeUIColorAndAlpha)
        return kUi;
    return -1;
}

const char* KindName(int k)
{
    static const char* names[kKinds] = { "depth", "motion vectors", "HUD-less", "UI" };
    return names[k];
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

// The copy texture of a slot and kind like source (made or remade; the replaced one released later). dataMutex held.
ID3D12Resource* CopyTarget(int slot, int kind, ID3D12Resource* source)
{
    const auto desc = source->GetDesc();
    auto& target = copies[slot][kind];

    if (target != nullptr)
    {
        const auto t = target->GetDesc();
        if (t.Width == desc.Width && t.Height == desc.Height && t.Format == desc.Format &&
            t.MipLevels == desc.MipLevels && t.DepthOrArraySize == desc.DepthOrArraySize &&
            t.SampleDesc.Count == desc.SampleDesc.Count && t.Dimension == desc.Dimension && t.Flags == desc.Flags)
        {
            return target;
        }

        retired.push_back({ target, kRetireFrames });
        target = nullptr;
    }

    ID3D12Device* device = nullptr;
    if (source->GetDevice(IID_PPV_ARGS(&device)) != S_OK || device == nullptr)
        return nullptr;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    // the same description: CopyResource needs it, and the image stays readable as the game's was
    auto hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&target));
    device->Release();

    if (hr != S_OK || target == nullptr)
    {
        LOG_ERROR("HelixSR DLSS-G inputs: no copy texture for the {} image: {:X}", KindName(kind), (UINT) hr);
        target = nullptr;
        return nullptr;
    }

    target->SetName(std::format(L"HelixSRDlssg_Copy[{}][{}]", slot, kind).c_str());
    return target;
}

void Matrix(float (&to)[16], const sl::float4x4& from) { memcpy(to, &from, sizeof(to)); }
} // namespace

void SetWanted(bool w) { wanted = w; }

void StoreConstants(int fIndex, uint32_t slFrame, const sl::Constants& c)
{
    std::scoped_lock lock(dataMutex);
    auto& s = constantsByFrame[slFrame % kConstantSlots];
    s.slFrame = slFrame;
    s.set = true;
    s.used = false;
    s.constants = c;

    if (fIndex >= 0 && fIndex < BUFFER_COUNT)
    {
        slFrameAtIndex[fIndex] = slFrame;
        slFrameAtIndexSet[fIndex] = true;
    }
}

void SetDlssgViewport(uint32_t viewport)
{
    std::scoped_lock lock(dataMutex);
    dlssgViewport = viewport;
    dlssgViewportSet = true;
}

void MarkPresent(uint32_t slFrame)
{
    std::scoped_lock lock(dataMutex);
    presentFrame = slFrame;
    presentSeen = true;

    for (auto it = retired.begin(); it != retired.end();)
    {
        if (--it->framesLeft > 0)
        {
            ++it;
            continue;
        }

        it->resource->Release();
        it = retired.erase(it);
    }
}

void Tag(uint32_t viewport, uint32_t slFrame, const sl::ResourceTag& tag, void* cmdBuffer)
{
    if (!wanted || State::Instance().currentFG == nullptr || tag.resource == nullptr || tag.resource->native == nullptr)
        return;

    const int kind = KindOf(tag.type);
    if (kind < 0)
        return;

    std::scoped_lock lock(dataMutex);

    if (dlssgViewportSet && viewport != dlssgViewport)
        return; // another viewport's image (sl.dlss_g reads its own viewport's tags)

    const int slot = static_cast<int>(slFrame % kTagSlots);
    auto& t = tags[slot];

    if (t.slFrame != slFrame)
    {
        t = {};
        t.slFrame = slFrame;
    }

    auto* res = static_cast<ID3D12Resource*>(tag.resource->native);
    auto* list = static_cast<ID3D12GraphicsCommandList*>(cmdBuffer);
    const auto desc = res->GetDesc();
    const auto state = static_cast<D3D12_RESOURCE_STATES>(tag.resource->state);

    HelixSRFfxDlssgResource r {};
    r.left = tag.extent ? tag.extent.left : 0;
    r.top = tag.extent ? tag.extent.top : 0;
    r.width = tag.extent ? tag.extent.width : static_cast<uint32_t>(desc.Width);
    r.height = tag.extent ? tag.extent.height : desc.Height;

    if (tag.lifecycle == sl::eValidUntilPresent || list == nullptr)
    {
        // the game's image, valid until this frame's present (without a command list nothing can be copied: as
        // OptiScaler, the image is taken as it is)
        r.resource = res;
        r.state = static_cast<uint32_t>(state);
    }
    else
    {
        // valid only now (or until evaluate): an exact copy on the tag's command list, as Streamline makes it
        auto* copy = CopyTarget(slot, kind, res);
        if (copy == nullptr)
            return;

        Barrier(list, res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(copy, res);
        Barrier(list, res, D3D12_RESOURCE_STATE_COPY_SOURCE, state);

        r.resource = copy;
        r.state = static_cast<uint32_t>(D3D12_RESOURCE_STATE_COPY_DEST);
        r.flags = HELIXSR_DLSSG_RES_COPY;
    }

    // the last tag of the frame wins (as in Streamline)
    t.res[kind] = r;
    t.set[kind] = true;
}

bool Fill(int fIndex, uint64_t frameID, const Options& options, HelixSRFfxDlssgInputs& x)
{
    if (State::Instance().activeFgInput != FGInput::DLSSG)
        return false;

    std::scoped_lock lock(dataMutex);

    // the frame: the one being presented (PCL marker), else the frame of the constants in OptiScaler's slot
    uint32_t slFrame = 0;
    const char* pairing = nullptr;

    if (presentSeen && constantsByFrame[presentFrame % kConstantSlots].set &&
        constantsByFrame[presentFrame % kConstantSlots].slFrame == presentFrame)
    {
        slFrame = presentFrame;
        pairing = "present marker";
    }
    else if (fIndex >= 0 && fIndex < BUFFER_COUNT && slFrameAtIndexSet[fIndex])
    {
        slFrame = slFrameAtIndex[fIndex];
        pairing = "FG slot";
    }
    else
    {
        return false;
    }

    auto& cs = constantsByFrame[slFrame % kConstantSlots];
    if (!cs.set || cs.slFrame != slFrame || cs.used)
    {
        static uint32_t told = 0;
        if (told++ < 3)
            LOG_DEBUG("HelixSR DLSS-G inputs: no new constants for Streamline frame {} (prepare {})", slFrame, frameID);
        return false;
    }

    const auto& ts = tags[slFrame % kTagSlots];
    if (ts.slFrame != slFrame || !ts.set[kDepth] || !ts.set[kMotionVectors])
    {
        static uint32_t told = 0;
        if (told++ < 3)
            LOG_DEBUG("HelixSR DLSS-G inputs: no depth / motion vectors tagged for Streamline frame {} (prepare {})",
                      slFrame, frameID);
        return false;
    }

    cs.used = true;
    const sl::Constants& c = cs.constants;

    x = {};
    x.type = HELIXSR_FFX_DESC_TYPE_DLSSG_INPUTS;
    x.structSize = sizeof(x);
    x.version = HELIXSR_FFX_DLSSG_INPUTS_VERSION;
    x.frameID = frameID;
    x.dlssgFrame = slFrame;

    x.depth = ts.res[kDepth];
    x.motionVectors = ts.res[kMotionVectors];

    if (options.hudless && ts.set[kHudless])
        x.hudlessColor = ts.res[kHudless];

    if (options.ui && ts.set[kUi])
        x.uiColor = ts.res[kUi];

    Matrix(x.cameraViewToClip, c.cameraViewToClip);
    Matrix(x.clipToCameraView, c.clipToCameraView);
    Matrix(x.clipToLensClip, c.clipToLensClip);
    Matrix(x.clipToPrevClip, c.clipToPrevClip);
    Matrix(x.prevClipToClip, c.prevClipToClip);

    x.jitterOffset[0] = c.jitterOffset.x;
    x.jitterOffset[1] = c.jitterOffset.y;
    x.mvecScale[0] = c.mvecScale.x;
    x.mvecScale[1] = c.mvecScale.y;
    x.cameraPinholeOffset[0] = c.cameraPinholeOffset.x;
    x.cameraPinholeOffset[1] = c.cameraPinholeOffset.y;
    memcpy(x.cameraPos, &c.cameraPos, sizeof(x.cameraPos));
    memcpy(x.cameraUp, &c.cameraUp, sizeof(x.cameraUp));
    memcpy(x.cameraRight, &c.cameraRight, sizeof(x.cameraRight));
    memcpy(x.cameraFwd, &c.cameraFwd, sizeof(x.cameraFwd));

    x.cameraNear = c.cameraNear;
    x.cameraFar = c.cameraFar;
    x.cameraFOV = c.cameraFOV;
    x.cameraAspectRatio = c.cameraAspectRatio;
    x.motionVectorsInvalidValue = c.motionVectorsInvalidValue;
    x.minRelativeLinearDepthObjectSeparation = c.minRelativeLinearDepthObjectSeparation;

    x.flags = (IsTrue(c.depthInverted) ? HELIXSR_DLSSG_DEPTH_INVERTED : 0u) |
              (IsTrue(c.cameraMotionIncluded) ? HELIXSR_DLSSG_CAMERA_MOTION_INCLUDED : 0u) |
              (IsTrue(c.motionVectors3D) ? HELIXSR_DLSSG_MOTION_VECTORS_3D : 0u) |
              (IsTrue(c.reset) ? HELIXSR_DLSSG_RESET : 0u) |
              (IsTrue(c.orthographicProjection) ? HELIXSR_DLSSG_ORTHOGRAPHIC : 0u) |
              (IsTrue(c.motionVectorsDilated) ? HELIXSR_DLSSG_MOTION_VECTORS_DILATED : 0u) |
              (IsTrue(c.motionVectorsJittered) ? HELIXSR_DLSSG_MOTION_VECTORS_JITTERED : 0u) |
              (x.uiColor.resource != nullptr && options.uiPremultiplied ? HELIXSR_DLSSG_UI_PREMULTIPLIED : 0u);

    static uint32_t logged = 0;
    if (logged++ < 3)
    {
        LOG_INFO("HelixSR DLSS-G inputs: prepare {} gets Streamline frame {} ({}): depth {}x{} at {},{}{}, motion "
                 "vectors {}x{} at {},{}{}, scale {} {}, jitter {} {}, near {}, far {}, separation {}, flags {:X}, "
                 "hudless {}, ui {}",
                 frameID, slFrame, pairing, x.depth.width, x.depth.height, x.depth.left, x.depth.top,
                 x.depth.flags ? " (copy)" : "", x.motionVectors.width, x.motionVectors.height, x.motionVectors.left,
                 x.motionVectors.top, x.motionVectors.flags ? " (copy)" : "", x.mvecScale[0], x.mvecScale[1],
                 x.jitterOffset[0], x.jitterOffset[1], x.cameraNear, x.cameraFar,
                 x.minRelativeLinearDepthObjectSeparation, x.flags, x.hudlessColor.resource != nullptr,
                 x.uiColor.resource != nullptr);
    }

    return true;
}

void Release()
{
    std::scoped_lock lock(dataMutex);

    for (auto& s : constantsByFrame)
        s = {};

    for (auto& t : tags)
        t = {};

    for (auto& set : slFrameAtIndexSet)
        set = false;

    presentSeen = false;
}
} // namespace HelixSRDlssg
