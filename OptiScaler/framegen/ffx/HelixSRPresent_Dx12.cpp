#include "pch.h"
#include "HelixSRPresent_Dx12.h"

#include <Config.h>
#include <State.h>
#include <NVNGX_Parameter.h>
#include <framegen/IFGFeature_Dx12.h>
#include <framegen/nvngx/Nvngx_FG.h>
#include <helixsr/HelixSR.h>

#include <nvsdk_ngx_defs.h>
#include <sl.h>
#include <sl_dlss_g.h>

// The NGX DLSS-G parameters are the keys and value types of Streamline's sl.dlss_g (as recorded from games and read
// by helixsr.dll from an OptiScaler parameter object); the values come from the game's sl::Constants, sl::DLSSGOptions
// and resource tags without any processing of our own, except the motion vector scale (Streamline's normalized scale
// times the motion vector size, as OptiScaler's other DLSSG input conversions do).

namespace HelixSRPresent
{
namespace
{
struct Input
{
    ID3D12Resource* resource = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    UINT left = 0;
    UINT top = 0;
    UINT width = 0;
    UINT height = 0;
};

std::mutex dataMutex;
sl::Constants constants[BUFFER_COUNT] {};
bool constantsSet[BUFFER_COUNT] {};
uint32_t optionFlags = 0;
uint32_t optionDynResWidth = 0;
uint32_t optionDynResHeight = 0;

// Present thread only (the FidelityFX frameGenerationCallback)
bool initDone = false;
NVNGX_Parameters* params = nullptr;
NVSDK_NGX_Handle* handle = nullptr;
UINT featureWidth = 0;
UINT featureHeight = 0;
DXGI_FORMAT featureFormat = DXGI_FORMAT_UNKNOWN;
uint32_t featureCountMax = 0; // DLSSG.MultiFrameCountMax the feature was created with
uint32_t backbufferFrameId = 0;
sl::Constants current {}; // the constants of the frame being generated (the matrices are passed by pointer)
ID3D12Resource* outputReal = nullptr;
bool hudlessSeen = false;
bool uiSeen = false;

// Async generation (StageInputs / Generate on a compute list): our copies of the game's inputs, one set. A new copy
// is recorded on the game queue only after the previous frame's generation finished (the swapchain's game queue wait
// on the interpolation fence), so one set is enough.
enum StagedSlot
{
    StagedDepth,
    StagedMvecs,
    StagedHudless,
    StagedUi,
    StagedCount
};
ID3D12Resource* stagedImages[StagedCount] {};
Input stagedInputs[StagedCount] {};
uint64_t stagedFrameID = 0;
bool stagedValid = false;

// Features and resources replaced (resize): released after enough frames for the GPU to be done with them
struct Retired
{
    NVSDK_NGX_Handle* handle = nullptr;
    ID3D12Resource* resource = nullptr;
    int framesLeft = 0;
};
std::vector<Retired> retired;

constexpr int kRetireFrames = BUFFER_COUNT + 4;

void Retire(NVSDK_NGX_Handle* h, ID3D12Resource* r)
{
    if (h != nullptr || r != nullptr)
        retired.push_back({ h, r, kRetireFrames });
}

void TickRetired()
{
    for (auto it = retired.begin(); it != retired.end();)
    {
        if (--it->framesLeft > 0)
        {
            ++it;
            continue;
        }

        if (it->handle != nullptr)
            Nvngx_FG::D3D12_ReleaseFeature(it->handle);

        if (it->resource != nullptr)
            it->resource->Release();

        it = retired.erase(it);
    }
}

int SlBool(sl::Boolean b) { return b == sl::Boolean::eTrue ? 1 : 0; }

bool IsHdrFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return true;
    default:
        return false;
    }
}

Input GetInput(IFGFeature_Dx12* fg, FG_ResourceType type, int fIndex)
{
    Input in {};
    auto r = fg->GetResource(type, fIndex); // shared lock held for this copy only

    if (r && r->GetResource() != nullptr && fg->IsResourceReady(type, fIndex))
    {
        in.resource = r->GetResource();
        in.state = r->state;
        in.left = r->left;
        in.top = r->top;

        const auto desc = in.resource->GetDesc();
        in.width = r->width > 0 ? static_cast<UINT>(r->width) : static_cast<UINT>(desc.Width);
        in.height = r->height > 0 ? r->height : desc.Height;
    }

    return in;
}

void SetSubrect(NVSDK_NGX_Parameter* p, const char* name, UINT x, UINT y, UINT w, UINT h)
{
    const std::string k = std::string("DLSSG.") + name;
    p->Set((k + "SubrectBaseX").c_str(), x);
    p->Set((k + "SubrectBaseY").c_str(), y);
    p->Set((k + "SubrectWidth").c_str(), w);
    p->Set((k + "SubrectHeight").c_str(), h);
}

void SetResource(NVSDK_NGX_Parameter* p, const char* name, const Input& in)
{
    const std::string k = std::string("DLSSG.") + name;
    p->Set(k.c_str(), (void*) in.resource);
    SetSubrect(p, name, in.left, in.top, in.width, in.height);
}

// Transitions to the states the NGX frame generation reads and writes in, and back afterwards
struct Transitions
{
    std::vector<D3D12_RESOURCE_BARRIER> to;

    void Add(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES target)
    {
        if (r == nullptr || from == target)
            return;

        for (const auto& b : to)
        {
            if (b.Transition.pResource == r)
                return; // one transition per resource
        }

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = target;
        to.push_back(b);
    }

    void Apply(ID3D12GraphicsCommandList* list) const
    {
        if (!to.empty())
            list->ResourceBarrier(static_cast<UINT>(to.size()), to.data());
    }

    void Revert(ID3D12GraphicsCommandList* list) const
    {
        if (to.empty())
            return;

        std::vector<D3D12_RESOURCE_BARRIER> back = to;
        for (auto& b : back)
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);

        list->ResourceBarrier(static_cast<UINT>(back.size()), back.data());
    }
};

// The states a compute command list may transition a resource from and to
bool ComputeLegal(D3D12_RESOURCE_STATES state)
{
    constexpr D3D12_RESOURCE_STATES kAllowed =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT | D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_COPY_SOURCE;

    // COMMON (= PRESENT) is 0
    return (state & ~kAllowed) == 0;
}

bool ComputeLegal(const Transitions& t)
{
    for (const auto& b : t.to)
    {
        if (!ComputeLegal(b.Transition.StateBefore) || !ComputeLegal(b.Transition.StateAfter))
        {
            LOG_ERROR("HelixSR FG present: state {:X} -> {:X} not allowed on a compute list",
                      (UINT) b.Transition.StateBefore, (UINT) b.Transition.StateAfter);
            return false;
        }
    }

    return true;
}

// An image of our own with the source's description (CopyResource needs the same size, format, mips and arrays),
// created in COPY_DEST
bool EnsureStagedImage(ID3D12Device* device, ID3D12Resource* source, ID3D12Resource*& image)
{
    auto desc = source->GetDesc();

    if (image != nullptr)
    {
        const auto d = image->GetDesc();
        if (d.Dimension == desc.Dimension && d.Width == desc.Width && d.Height == desc.Height &&
            d.DepthOrArraySize == desc.DepthOrArraySize && d.MipLevels == desc.MipLevels && d.Format == desc.Format &&
            d.SampleDesc.Count == desc.SampleDesc.Count)
        {
            return true;
        }

        Retire(nullptr, image);
        image = nullptr;
    }

    desc.Layout = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                     : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Alignment = 0;
    desc.Flags &= ~(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                    D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&image));

    if (hr != S_OK || image == nullptr)
    {
        LOG_ERROR("HelixSR FG present: input copy ({}x{} format {}) not created: {:X}", desc.Width, desc.Height,
                  (UINT) desc.Format, (UINT) hr);
        image = nullptr;
        return false;
    }

    image->SetName(L"HelixSRPresent_StagedInput");
    return true;
}

bool EnsureOutputReal(ID3D12Device* device, ID3D12Resource* like)
{
    const auto likeDesc = like->GetDesc();

    if (outputReal != nullptr)
    {
        const auto d = outputReal->GetDesc();
        if (d.Width == likeDesc.Width && d.Height == likeDesc.Height && d.Format == likeDesc.Format)
            return true;

        Retire(nullptr, outputReal);
        outputReal = nullptr;
    }

    D3D12_RESOURCE_DESC desc = likeDesc;
    desc.MipLevels = 1;
    desc.DepthOrArraySize = 1;
    desc.SampleDesc = { 1, 0 };
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                              nullptr, IID_PPV_ARGS(&outputReal));

    if (hr != S_OK || outputReal == nullptr)
    {
        LOG_ERROR("HelixSR FG present: OutputReal texture not created: {:X}", (UINT) hr);
        outputReal = nullptr;
        return false;
    }

    outputReal->SetName(L"HelixSRPresent_OutputReal");
    return true;
}

bool EnsureFeature(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer, const Input& depth, uint32_t countMax)
{
    const auto bb = backbuffer->GetDesc();

    if (handle != nullptr && featureWidth == bb.Width && featureHeight == bb.Height && featureFormat == bb.Format &&
        featureCountMax == countMax)
        return true;

    if (handle != nullptr)
    {
        LOG_INFO("HelixSR FG present: backbuffer {}x{} format {}, up to {} generated frame(s): frame generation recreated",
                 bb.Width, bb.Height, (UINT) bb.Format, countMax);
        Retire(handle, nullptr);
        handle = nullptr;
    }

    if (params == nullptr)
        params = GetNGXParameters(API::DX12, true);

    // sl.dlss_g's creation keys
    const UINT internalWidth = optionDynResWidth > 0 ? optionDynResWidth : depth.width;
    const UINT internalHeight = optionDynResHeight > 0 ? optionDynResHeight : depth.height;

    params->Set("Width", static_cast<unsigned int>(bb.Width));
    params->Set("Height", static_cast<unsigned int>(bb.Height));
    params->Set("CreationNodeMask", 1u);
    params->Set("VisibilityNodeMask", 1u);
    params->Set("DLSSG.Width", static_cast<unsigned int>(bb.Width));
    params->Set("DLSSG.Height", static_cast<unsigned int>(bb.Height));
    params->Set("DLSSG.BackbufferFormat", static_cast<unsigned int>(bb.Format));
    params->Set("DLSSG.InternalWidth", internalWidth);
    params->Set("DLSSG.InternalHeight", internalHeight);
    params->Set("DLSSG.MultiFrameCountMax", countMax);

    NVSDK_NGX_Handle* h = nullptr;
    const auto result = Nvngx_FG::D3D12_CreateFeature(list, NVSDK_NGX_Feature_FrameGeneration, params, &h);

    if (result != NVSDK_NGX_Result_Success || h == nullptr)
    {
        LOG_ERROR("HelixSR FG present: frame generation feature not created: {:X}", (UINT) result);
        return false;
    }

    handle = h;
    featureWidth = static_cast<UINT>(bb.Width);
    featureHeight = bb.Height;
    featureFormat = bb.Format;
    featureCountMax = countMax;
    backbufferFrameId = 0;

    LOG_INFO("HelixSR FG present: frame generation created at {}x{} (internal {}x{}), up to {} generated frame(s)",
             bb.Width, bb.Height, internalWidth, internalHeight, countMax);
    return true;
}
} // namespace

bool Requested()
{
    auto v = Config::Instance()->HelixSRFGPresent.value_or("streamline");
    return v == "optiscaler";
}

bool Labeled()
{
    const auto& state = State::Instance();
    return Requested() && state.activeFgInput == FGInput::DLSSG && state.activeFgOutput == FGOutput::FSRFG &&
           state.activeFgNvngx == FGNvngxReplacement::HelixSR;
}

bool Active() { return Labeled() && State::Instance().currentFG != nullptr; }

void StoreConstants(int fIndex, const sl::Constants& c)
{
    if (fIndex < 0 || fIndex >= BUFFER_COUNT)
        return;

    std::scoped_lock lock(dataMutex);
    constants[fIndex] = c;
    constantsSet[fIndex] = true;
}

void StoreOptions(const sl::DLSSGOptions& options)
{
    std::scoped_lock lock(dataMutex);
    optionFlags = static_cast<uint32_t>(options.flags);
    optionDynResWidth = options.dynamicResWidth;
    optionDynResHeight = options.dynamicResHeight;
}

void Init(ID3D12Device* device)
{
    if (initDone || device == nullptr || !Requested())
        return;

    if (State::Instance().activeFgNvngx != FGNvngxReplacement::HelixSR || !Nvngx_FG::isDx12Available())
    {
        LOG_WARN("HelixSR FG present: HelixSR's frame generation (helixsr.dll with helixsr-fg) is not available");
        return;
    }

    // helixsr.dll counts inits: a second one (the game's NGX init) is harmless
    const auto path = HelixSR::Found() ? HelixSR::Folder().wstring() : Config::Instance()->MainDllPath.value();
    const auto result = Nvngx_FG::D3D12_Init_Ext(0x1337, path.c_str(), device, NVSDK_NGX_Version_API, nullptr);

    initDone = result == NVSDK_NGX_Result_Success;
    LOG_INFO("HelixSR FG present: NGX init {:X}", (UINT) result);
}

bool StageInputs(IFGFeature_Dx12* fg, ID3D12GraphicsCommandList* gameList, uint64_t frameID)
{
    stagedValid = false;

    if (fg == nullptr || gameList == nullptr)
        return false;

    const int fIndex = static_cast<int>(frameID % BUFFER_COUNT);

    const Input sources[StagedCount] = {
        GetInput(fg, FG_ResourceType::Depth, fIndex),
        GetInput(fg, FG_ResourceType::Velocity, fIndex),
        GetInput(fg, FG_ResourceType::HudlessColor, fIndex),
        GetInput(fg, FG_ResourceType::UIColor, fIndex),
    };

    ID3D12Device* device = nullptr;
    if (gameList->GetDevice(IID_PPV_ARGS(&device)) != S_OK || device == nullptr)
        return false;

    bool ok = true;
    for (int i = 0; i < StagedCount && ok; i++)
    {
        if (sources[i].resource != nullptr)
            ok = EnsureStagedImage(device, sources[i].resource, stagedImages[i]);
    }

    device->Release();

    if (!ok)
        return false;

    // the game's images: from their state (valid on the game queue at Present) to COPY_SOURCE and back; ours stay in
    // COPY_DEST (Generate moves them to NON_PIXEL_SHADER_RESOURCE and back on the compute list)
    Transitions t;
    for (int i = 0; i < StagedCount; i++)
    {
        if (sources[i].resource != nullptr)
            t.Add(sources[i].resource, sources[i].state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }

    t.Apply(gameList);

    for (int i = 0; i < StagedCount; i++)
    {
        stagedInputs[i] = sources[i];

        if (sources[i].resource == nullptr)
            continue;

        gameList->CopyResource(stagedImages[i], sources[i].resource);
        stagedInputs[i].resource = stagedImages[i];
        stagedInputs[i].state = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    t.Revert(gameList);

    stagedFrameID = frameID;
    stagedValid = true;
    return true;
}

uint32_t MaxGenerated()
{
    if (State::Instance().activeFgNvngx != FGNvngxReplacement::HelixSR)
        return 1;

    return static_cast<uint32_t>(std::clamp(Nvngx_FG::getMaxFakeFramesCount(), 1, static_cast<int>(kMaxGenerated)));
}

bool Generate(IFGFeature_Dx12* fg, const Frame& f, bool generated[kMaxGenerated])
{
    for (uint32_t i = 0; i < kMaxGenerated; i++)
        generated[i] = false;

    TickRetired();

    const uint32_t count = std::clamp(f.count, 1u, kMaxGenerated);

    if (fg == nullptr || f.cmdList == nullptr || f.backbuffer == nullptr)
        return false;

    for (uint32_t i = 0; i < count; i++)
    {
        if (f.outputs[i] == nullptr)
            return false;
    }

    const int fIndex = static_cast<int>(f.frameID % BUFFER_COUNT);
    auto& state = State::Instance();
    auto config = Config::Instance();

    ID3D12Device* device = nullptr;
    if (f.cmdList->GetDevice(IID_PPV_ARGS(&device)) != S_OK || device == nullptr)
        return false;

    if (!initDone)
        Init(device);

    if (!initDone)
    {
        device->Release();
        return false;
    }

    uint32_t flags = 0;
    {
        std::scoped_lock lock(dataMutex);
        if (!constantsSet[fIndex])
        {
            device->Release();
            LOG_WARN("HelixSR FG present: no Streamline constants for frame {}", f.frameID);
            return false;
        }

        current = constants[fIndex];
        flags = optionFlags;
    }

    // On a compute list (async generation) only our copies of this frame's inputs are read (StageInputs, recorded on
    // the game queue before the generation queue's wait on the game's frame)
    const bool computeList = f.cmdList->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE;
    if (computeList && (!stagedValid || stagedFrameID != f.frameID))
    {
        device->Release();
        LOG_WARN("HelixSR FG present: no input copies for frame {} on the compute queue", f.frameID);
        return false;
    }

    const Input depth = computeList ? stagedInputs[StagedDepth] : GetInput(fg, FG_ResourceType::Depth, fIndex);
    const Input mvecs = computeList ? stagedInputs[StagedMvecs] : GetInput(fg, FG_ResourceType::Velocity, fIndex);
    const Input hudless =
        computeList ? stagedInputs[StagedHudless] : GetInput(fg, FG_ResourceType::HudlessColor, fIndex);
    const Input ui = computeList ? stagedInputs[StagedUi] : GetInput(fg, FG_ResourceType::UIColor, fIndex);

    // The required inputs: never generate with a missing binding
    if (depth.resource == nullptr || mvecs.resource == nullptr)
    {
        device->Release();
        LOG_WARN("HelixSR FG present: depth or motion vectors missing for frame {}", f.frameID);
        return false;
    }

    // A game that tags HUD-less / UI images and misses them this frame: no generation without them
    if ((hudlessSeen && hudless.resource == nullptr) || (uiSeen && ui.resource == nullptr))
    {
        device->Release();
        LOG_DEBUG("HelixSR FG present: HUD-less or UI image missing for frame {}", f.frameID);
        return false;
    }

    hudlessSeen = hudlessSeen || hudless.resource != nullptr;
    uiSeen = uiSeen || ui.resource != nullptr;

    const bool outputRealOk = EnsureOutputReal(device, f.outputs[0]);
    device->Release();

    // created for the most frames the game may ask for, so that switching between 2x / 3x / 4x keeps the feature
    const uint32_t countMax = (std::max)(count, MaxGenerated());

    if (!outputRealOk || !EnsureFeature(f.cmdList, f.backbuffer, depth, countMax))
        return false;

    const auto bb = f.backbuffer->GetDesc();
    auto* p = params;

    // sl.dlss_g's evaluation keys (helixsr.dll's list of the keys Streamline sets). The ones that are the same for
    // every generated frame of this real frame are set once; the frame id moves on once per real frame.
    p->Set("Enable.OFA", 1);
    p->Set("DLSSG.BackbufferFrameID", backbufferFrameId++);
    p->Set("DLSSG.EvalFlags", 0ull);
    p->Set("DLSSG.EnableInterp", 1);
    p->Set("DLSSG.IsRecording", 1u);
    p->Set("DLSSG.NotRenderingGameFrames", 0);
    p->Set("DLSSG.MultiFrameCount", count);
    p->Set("DLSSG.MultiFrameCountMax", countMax);
    p->Set("DLSSG.ColorBuffersHDR", (state.hdrOutputActive || IsHdrFormat(bb.Format)) ? 1 : 0);
    p->Set("DLSSG.DepthInverted", SlBool(current.depthInverted));
    p->Set("DLSSG.CameraMotionIncluded", SlBool(current.cameraMotionIncluded));
    p->Set("DLSSG.OrthoProjection", SlBool(current.orthographicProjection));
    p->Set("DLSSG.DynamicResolution", (flags & (uint32_t) sl::DLSSGFlags::eDynamicResolutionEnabled) ? 1 : 0);
    p->Set("DLSSG.MvecDilated", SlBool(current.motionVectorsDilated));
    p->Set("DLSSG.MvecJittered", SlBool(current.motionVectorsJittered));
    p->Set("DLSSG.MvecInvalidValue", current.motionVectorsInvalidValue);
    p->Set("DLSSG.MvecScaleX", current.mvecScale.x * static_cast<float>(mvecs.width));
    p->Set("DLSSG.MvecScaleY", current.mvecScale.y * static_cast<float>(mvecs.height));
    p->Set("DLSSG.JitterOffsetX", current.jitterOffset.x);
    p->Set("DLSSG.JitterOffsetY", current.jitterOffset.y);
    p->Set("DLSSG.MenuDetectionEnabled", (flags & (uint32_t) sl::DLSSGFlags::eEnableFullscreenMenuDetection) ? 1 : 0);
    p->Set("DLSSG.AutomodeOverrideReset", 0);
    p->Set("DLSSG.run_lowres_mvec_pass", 1);
    p->Set("DLSSG.MinRelativeLinearDepthObjectSeparation", current.minRelativeLinearDepthObjectSeparation);
    p->Set("DLSSG.BidirectionalDistortionFieldLowPrecision.IsLowPrecision", 0);
    p->Set("DLSSG.BidirectionalDistortionFieldLowPrecision.Scale", 1.0f);
    p->Set("DLSSG.BidirectionalDistortionFieldLowPrecision.Bias", 0.0f);
    p->Set("DLSSG.CameraNear", current.cameraNear);
    p->Set("DLSSG.CameraFar", current.cameraFar);
    p->Set("DLSSG.CameraFOV", current.cameraFOV);
    p->Set("DLSSG.CameraAspectRatio", current.cameraAspectRatio);
    p->Set("DLSSG.CameraPinholeOffsetX", current.cameraPinholeOffset.x);
    p->Set("DLSSG.CameraPinholeOffsetY", current.cameraPinholeOffset.y);
    p->Set("DLSSG.CameraPosX", current.cameraPos.x);
    p->Set("DLSSG.CameraPosY", current.cameraPos.y);
    p->Set("DLSSG.CameraPosZ", current.cameraPos.z);
    p->Set("DLSSG.CameraUpX", current.cameraUp.x);
    p->Set("DLSSG.CameraUpY", current.cameraUp.y);
    p->Set("DLSSG.CameraUpZ", current.cameraUp.z);
    p->Set("DLSSG.CameraRightX", current.cameraRight.x);
    p->Set("DLSSG.CameraRightY", current.cameraRight.y);
    p->Set("DLSSG.CameraRightZ", current.cameraRight.z);
    p->Set("DLSSG.CameraFwdX", current.cameraFwd.x);
    p->Set("DLSSG.CameraFwdY", current.cameraFwd.y);
    p->Set("DLSSG.CameraFwdZ", current.cameraFwd.z);
    p->Set("DLSSG.CameraViewToClip", (void*) &current.cameraViewToClip);
    p->Set("DLSSG.ClipToCameraView", (void*) &current.clipToCameraView);
    p->Set("DLSSG.ClipToLensClip", (void*) &current.clipToLensClip);
    p->Set("DLSSG.ClipToPrevClip", (void*) &current.clipToPrevClip);
    p->Set("DLSSG.PrevClipToClip", (void*) &current.prevClipToClip);

    // Resources: the real frame with the HUD, the game's tagged inputs (the generated frames per evaluation below)
    Input backbuffer { f.backbuffer, f.backbufferState, 0, 0, static_cast<UINT>(bb.Width), bb.Height };
    Input real { outputReal, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, 0, static_cast<UINT>(bb.Width), bb.Height };

    SetResource(p, "Backbuffer", backbuffer);
    SetResource(p, "Depth", depth);
    SetResource(p, "MVecs", mvecs);
    SetResource(p, "OutputReal", real);

    // HUD-less and UI exactly as the game tags them; a game that never tags them gets none (as from Streamline). The
    // FG output's FGDisableHudless / FGDisableUI already keep them out. Not "none" for a single frame: that changes
    // how the HUD is generated, the real frame is shown instead.
    if (hudless.resource != nullptr)
        SetResource(p, "HUDLess", hudless);
    else
        p->Set("DLSSG.HUDLess", (void*) nullptr);

    if (ui.resource != nullptr)
        SetResource(p, "UI", ui);
    else
        p->Set("DLSSG.UI", (void*) nullptr);

    SetSubrect(p, "NoWarp", 0, 0, 0, 0);
    SetSubrect(p, "BidirectionalDistortionField", 0, 0, 0, 0);

    // States as with Streamline: inputs readable by compute, outputs writable. Nvngx_FG's HUD cutoff (the same as with
    // Streamline presenting, at the first index only) expects the HUD-less image in COPY_DEST.
    const bool hudCutoff =
        config->FGHudCutoff.value_or_default() > 0.0f || state.gameQuirks & GameQuirk::FSRFGHudlessMismatchFixup;
    constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    Transitions t;
    for (uint32_t i = 0; i < count; i++)
        t.Add(f.outputs[i], f.outputStates[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    t.Add(f.backbuffer, f.backbufferState, kRead);
    t.Add(depth.resource, depth.state, kRead);
    t.Add(mvecs.resource, mvecs.state, kRead);
    t.Add(hudless.resource, hudless.state, hudCutoff ? D3D12_RESOURCE_STATE_COPY_DEST : kRead);
    t.Add(ui.resource, ui.state, kRead);

    // a compute list: every transition between states it may use (our copies, our images, the swapchain's real
    // frame in PRESENT)
    if (computeList && !ComputeLegal(t))
        return false;

    t.Apply(f.cmdList);

    // One evaluation per generated frame, in present order, on this command list (as Streamline records them):
    // index 1 also runs the once-per-real-frame part (and the HUD cutoff), the last one moves the history on. The
    // reset applies to the first. From an index that fails on, the slots show the real frame (later indices are not
    // evaluated: a generated frame after a real one would step back in time).
    bool any = false;
    const bool reset = SlBool(current.reset) || f.reset;
    NVSDK_NGX_Result lastResult = NVSDK_NGX_Result_Success;

    for (uint32_t i = 0; i < count; i++)
    {
        Input output { f.outputs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, 0, static_cast<UINT>(bb.Width), bb.Height };
        SetResource(p, "OutputInterpolated", output);
        p->Set("DLSSG.MultiFrameIndex", i + 1);
        p->Set("DLSSG.Reset", (reset && i == 0) ? 1 : 0);

        // The same call as with Streamline presenting: Nvngx_FG (HUD cutoff) -> helixsr.dll -> NVIDIA's core
        lastResult = Nvngx_FG::D3D12_EvaluateFeature(f.cmdList, handle, p, nullptr);
        generated[i] = lastResult == NVSDK_NGX_Result_Success;
        any = any || generated[i];

        if (!generated[i])
        {
            // the later frames lie after this one: the caller shows the real frame from here on
            LOG_WARN("HelixSR FG present: frame {} index {} of {} not generated: {:X}", f.frameID, i + 1, count,
                     (UINT) lastResult);
            break;
        }
    }

    t.Revert(f.cmdList);

    static uint32_t logged = 0;
    static uint32_t loggedCount = 0;
    if (logged < 3 || count != loggedCount)
    {
        logged++;
        loggedCount = count;
        LOG_INFO("HelixSR FG present: frame {}: {} generated frame(s), last result {:X} (depth {}x{}, mvecs {}x{}, "
                 "hudless {}, ui {})",
                 f.frameID, count, (UINT) lastResult, depth.width, depth.height, mvecs.width, mvecs.height,
                 hudless.resource != nullptr, ui.resource != nullptr);
    }

    return any;
}

void Release()
{
    Retire(handle, outputReal);
    handle = nullptr;
    outputReal = nullptr;
    featureCountMax = 0;
    featureWidth = 0;
    featureHeight = 0;
    featureFormat = DXGI_FORMAT_UNKNOWN;
    hudlessSeen = false;
    uiSeen = false;

    for (auto& image : stagedImages)
    {
        Retire(nullptr, image);
        image = nullptr;
    }
    stagedValid = false;

    std::scoped_lock lock(dataMutex);
    for (auto& set : constantsSet)
        set = false;
}
} // namespace HelixSRPresent
