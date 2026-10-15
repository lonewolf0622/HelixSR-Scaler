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
uint32_t backbufferFrameId = 0;
sl::Constants current {}; // the constants of the frame being generated (the matrices are passed by pointer)
ID3D12Resource* outputReal = nullptr;
bool hudlessSeen = false;
bool uiSeen = false;

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

bool EnsureFeature(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer, const Input& depth)
{
    const auto bb = backbuffer->GetDesc();

    if (handle != nullptr && featureWidth == bb.Width && featureHeight == bb.Height && featureFormat == bb.Format)
        return true;

    if (handle != nullptr)
    {
        LOG_INFO("HelixSR FG present: backbuffer changed to {}x{} format {}, frame generation recreated", bb.Width,
                 bb.Height, (UINT) bb.Format);
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
    params->Set("DLSSG.MultiFrameCountMax", 1u);

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
    backbufferFrameId = 0;

    LOG_INFO("HelixSR FG present: frame generation created at {}x{} (internal {}x{})", bb.Width, bb.Height,
             internalWidth, internalHeight);
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

bool Generate(IFGFeature_Dx12* fg, const Frame& f)
{
    TickRetired();

    if (fg == nullptr || f.cmdList == nullptr || f.backbuffer == nullptr || f.output == nullptr)
        return false;

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

    const Input depth = GetInput(fg, FG_ResourceType::Depth, fIndex);
    const Input mvecs = GetInput(fg, FG_ResourceType::Velocity, fIndex);
    const Input hudless = GetInput(fg, FG_ResourceType::HudlessColor, fIndex);
    const Input ui = GetInput(fg, FG_ResourceType::UIColor, fIndex);

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

    const bool outputRealOk = EnsureOutputReal(device, f.output);
    device->Release();

    if (!outputRealOk || !EnsureFeature(f.cmdList, f.backbuffer, depth))
        return false;

    const auto bb = f.backbuffer->GetDesc();
    auto* p = params;

    // sl.dlss_g's evaluation keys (helixsr.dll's list of the keys Streamline sets)
    p->Set("Enable.OFA", 1);
    p->Set("DLSSG.BackbufferFrameID", backbufferFrameId++);
    p->Set("DLSSG.EvalFlags", 0ull);
    p->Set("DLSSG.EnableInterp", 1);
    p->Set("DLSSG.Reset", (SlBool(current.reset) || f.reset) ? 1 : 0);
    p->Set("DLSSG.IsRecording", 1u);
    p->Set("DLSSG.NotRenderingGameFrames", 0);
    p->Set("DLSSG.MultiFrameCount", 1u);
    p->Set("DLSSG.MultiFrameIndex", 1u);
    p->Set("DLSSG.MultiFrameCountMax", 1u);
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

    // Resources: the real frame with the HUD, the game's tagged inputs, the generated frame
    Input backbuffer { f.backbuffer, f.backbufferState, 0, 0, static_cast<UINT>(bb.Width), bb.Height };
    Input output { f.output, f.outputState, 0, 0, static_cast<UINT>(bb.Width), bb.Height };
    Input real { outputReal, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 0, 0, static_cast<UINT>(bb.Width), bb.Height };

    SetResource(p, "Backbuffer", backbuffer);
    SetResource(p, "Depth", depth);
    SetResource(p, "MVecs", mvecs);
    SetResource(p, "OutputInterpolated", output);
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
    // Streamline presenting) expects the HUD-less image in COPY_DEST.
    const bool hudCutoff =
        config->FGHudCutoff.value_or_default() > 0.0f || state.gameQuirks & GameQuirk::FSRFGHudlessMismatchFixup;
    constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    Transitions t;
    t.Add(f.output, f.outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    t.Add(f.backbuffer, f.backbufferState, kRead);
    t.Add(depth.resource, depth.state, kRead);
    t.Add(mvecs.resource, mvecs.state, kRead);
    t.Add(hudless.resource, hudless.state, hudCutoff ? D3D12_RESOURCE_STATE_COPY_DEST : kRead);
    t.Add(ui.resource, ui.state, kRead);
    t.Apply(f.cmdList);

    // The same call as with Streamline presenting: Nvngx_FG (HUD cutoff) -> helixsr.dll -> NVIDIA's core
    const auto result = Nvngx_FG::D3D12_EvaluateFeature(f.cmdList, handle, p, nullptr);

    t.Revert(f.cmdList);

    static uint32_t logged = 0;
    if (logged < 3 || result != NVSDK_NGX_Result_Success)
    {
        logged++;
        LOG_INFO("HelixSR FG present: frame {} generated: {:X} (depth {}x{}, mvecs {}x{}, hudless {}, ui {})",
                 f.frameID, (UINT) result, depth.width, depth.height, mvecs.width, mvecs.height,
                 hudless.resource != nullptr, ui.resource != nullptr);
    }

    return result == NVSDK_NGX_Result_Success;
}

void Release()
{
    Retire(handle, outputReal);
    handle = nullptr;
    outputReal = nullptr;
    featureWidth = 0;
    featureHeight = 0;
    featureFormat = DXGI_FORMAT_UNKNOWN;
    hudlessSeen = false;
    uiSeen = false;

    std::scoped_lock lock(dataMutex);
    for (auto& set : constantsSet)
        set = false;
}
} // namespace HelixSRPresent
