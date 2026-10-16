// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

// HelixSR Scaler: HelixSR's presenting swapchain, adapted from the FidelityFX SDK's frame interpolation swapchain
// (framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12.{h,cpp} and FrameInterpolationSwapchainDX12_Helpers.{h,cpp},
// MIT, notice above). Changes from AMD's version:
// - up to three generated frames per real frame (multi-frame generation): frame types Generated_1..3 + Real, two sets
//   of three output images, the frame generation callback fills outputs[0..N-1] (numGeneratedFrames = N) and the
//   presenter shows the N generated frames, then the real one, each paced at the real frame time / (N + 1)
// - its own configuration (setConfig) instead of the FidelityFX API context; no UI composition, no debug pacing lines,
//   no registered interpolation command lists: every frame is copied as it is into the swapchain image
// - the interpolation fence is signaled after the generation queue waited for the game's frame even when the frame
//   generation callback fails, so the real frame is never shown before the game finished rendering it
// - generation on the async compute queue (allowAsyncWorkloads): the game's inputs are first copied on the game queue
//   (stageInputsCallback), the compute list only reads those copies; no copies, no generation for that frame
// See HELIXSR_FG_PRESENT.md (multi-frame generation) for the fence review.

#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <ffx_api.h>
#include <ffx_framegeneration.h>

namespace hxsc
{
constexpr UINT kMaxBufferCount = DXGI_MAX_SWAP_CHAIN_BUFFERS;
constexpr UINT kMaxGeneratedFrames = 3; // DLSS 4's 4x
constexpr UINT kOutputSets = 2;         // generation of the next real frame writes one set while the other is shown

typedef int32_t (*WaitCallbackFunc)(wchar_t* fenceName, uint64_t fenceValueToWaitFor);

constexpr UINT kUnknownTimerResolution = 0;

template <class Interface> inline UINT SafeRelease(Interface*& p)
{
    UINT refCount = (std::numeric_limits<UINT>::max)();
    if (p != nullptr)
    {
        refCount = p->Release();
        p = nullptr;
    }
    return refCount;
}

inline void SafeCloseHandle(HANDLE& handle)
{
    if (handle)
    {
        CloseHandle(handle);
        handle = 0;
    }
}

class Dx12Commands
{
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 availableFenceValue = 0;

  public:
    void release()
    {
        SafeRelease(allocator);
        SafeRelease(list);
        SafeRelease(fence);
    }

    ~Dx12Commands() { release(); }

    bool initiated() { return allocator != nullptr; }

    bool verify(ID3D12CommandQueue* pQueue)
    {
        HRESULT hr = initiated() ? S_OK : E_FAIL;

        if (FAILED(hr))
        {
            ID3D12Device* device = nullptr;
            if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&device))))
            {
                D3D12_COMMAND_QUEUE_DESC queueDesc = pQueue->GetDesc();
                if (SUCCEEDED(device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator))))
                {
                    if (SUCCEEDED(device->CreateCommandList(queueDesc.NodeMask, queueDesc.Type, allocator, nullptr,
                                                            IID_PPV_ARGS(&list))))
                    {
                        list->Close();

                        if (SUCCEEDED(device->CreateFence(availableFenceValue, D3D12_FENCE_FLAG_NONE,
                                                          IID_PPV_ARGS(&fence))))
                        {
                            hr = S_OK;
                        }
                    }
                }

                SafeRelease(device);
            }
        }

        if (FAILED(hr))
            release();

        return SUCCEEDED(hr);
    }

    void occupy(ID3D12CommandQueue* pQueue, const wchar_t* name)
    {
        availableFenceValue++;
        queue = pQueue;
        allocator->SetName(name);
        list->SetName(name);
        fence->SetName(name);
    }

    ID3D12GraphicsCommandList* reset()
    {
        if (SUCCEEDED(allocator->Reset()))
            list->Reset(allocator, nullptr);

        return list;
    }

    ID3D12GraphicsCommandList* getList() { return list; }

    void execute(bool listIsOpen = false)
    {
        if (listIsOpen)
            list->Close();

        ID3D12CommandList* pListsToExec[] = { list };
        queue->ExecuteCommandLists(_countof(pListsToExec), pListsToExec);
        queue->Signal(fence, availableFenceValue);
    }

    void drop(bool listIsOpen = false)
    {
        if (listIsOpen)
            getList()->Close();

        queue->Signal(fence, availableFenceValue);
    }

    bool available() { return fence->GetCompletedValue() >= availableFenceValue; }
};

template <size_t Capacity> class Dx12CommandPool
{
    CRITICAL_SECTION criticalSection {};
    Dx12Commands buffer[4 /* D3D12_COMMAND_LIST_TYPE_COPY == 3 */][Capacity] = {};

  public:
    Dx12CommandPool() { InitializeCriticalSection(&criticalSection); }

    ~Dx12CommandPool()
    {
        EnterCriticalSection(&criticalSection);

        for (size_t type = 0; type < 4; type++)
        {
            for (size_t idx = 0; idx < Capacity; idx++)
            {
                auto& cmds = buffer[type][idx];
                while (cmds.initiated() && !cmds.available())
                {
                    // wait for the list to be idle
                }
                cmds.release();
            }
        }

        LeaveCriticalSection(&criticalSection);
        DeleteCriticalSection(&criticalSection);
    }

    // nullptr: every list of this queue type is still in flight
    Dx12Commands* get(ID3D12CommandQueue* pQueue, const wchar_t* name)
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc = pQueue->GetDesc();

        EnterCriticalSection(&criticalSection);

        Dx12Commands* pCommands = nullptr;
        for (size_t idx = 0; idx < Capacity && (pCommands == nullptr); idx++)
        {
            auto& cmds = buffer[queueDesc.Type][idx];
            if (cmds.verify(pQueue) && cmds.available())
                pCommands = &cmds;
        }

        if (pCommands != nullptr)
            pCommands->occupy(pQueue, name);

        LeaveCriticalSection(&criticalSection);

        return pCommands;
    }
};

template <const int Size, typename Type = double> struct SimpleMovingAverage
{
    Type history[Size] = {};
    unsigned int idx = 0;
    unsigned int updateCount = 0;

    Type getAverage()
    {
        if (updateCount < Size)
            return 0.0;

        Type average = 0.f;
        for (size_t i = 0; i < Size; i++)
            average += history[i];

        return average / Size;
    }

    Type getVariance()
    {
        if (updateCount < Size)
            return 0.0;

        Type average = getAverage();
        Type variance = 0.f;
        for (size_t i = 0; i < Size; i++)
            variance += (history[i] - average) * (history[i] - average);

        return std::sqrt(variance / Size);
    }

    void reset()
    {
        updateCount = 0;
        idx = 0;
    }

    void update(Type newValue)
    {
        history[idx] = newValue;
        idx = (idx + 1) % Size;
        updateCount++;
    }
};

// One real frame's presents: Generated_1 .. Generated_N, then Real
struct PacingData
{
    bool vsync;
    bool tearingSupported;

    UINT64 interpolationCompletedFenceValue;
    UINT64 replacementBufferFenceSignal;
    UINT64 numFramesSentForPresentationBase;
    UINT32 numFramesToPresent;
    UINT64 currentFrameID;

    enum FrameType
    {
        Generated_1,
        Generated_2,
        Generated_3,
        Real,
        Count
    };

    struct FrameInfo
    {
        bool doPresent;
        ID3D12Resource* resource;
        D3D12_RESOURCE_STATES state;
        UINT64 interpolationCompletedFenceValue;
        UINT64 presentIndex;
        int64_t presentQpcDelta;
    };

    FrameInfo frames[FrameType::Count];

    void invalidate() { memset(this, 0, sizeof(PacingData)); }
};

struct PresentInfo
{
    CRITICAL_SECTION criticalSectionScheduledFrame;
    ID3D12Device* device = nullptr;
    IDXGISwapChain4* swapChain = nullptr;
    Dx12CommandPool<32> commandPool;

    PacingData scheduledInterpolations;
    PacingData scheduledPresents;

    ID3D12CommandQueue* interpolationQueue = nullptr;
    ID3D12CommandQueue* asyncComputeQueue = nullptr;
    ID3D12CommandQueue* gameQueue = nullptr;
    ID3D12CommandQueue* presentQueue = nullptr;

    ID3D12Fence* gameFence = nullptr;
    ID3D12Fence* interpolationFence = nullptr;
    ID3D12Fence* presentFence = nullptr;
    ID3D12Fence* replacementBufferFence = nullptr;
    ID3D12Fence* compositionFenceCPU = nullptr;
    ID3D12Fence* compositionFenceGPU = nullptr;

    HANDLE presentEvent = 0;
    HANDLE interpolationEvent = 0;
    HANDLE pacerEvent = 0;

    volatile bool resetTimer = false;
    volatile bool shutdown = false;

    volatile double safetyMarginInSec = 0.0001; // 0.1 ms
    volatile double varianceFactor = 0.1;
    volatile bool allowHybridSpin = false;
    volatile uint32_t hybridSpinTime = 2;
    volatile bool allowWaitForSingleObjectOnFence = false;

    WaitCallbackFunc waitCallback = nullptr;

    volatile int64_t previousPresentQpc = 0;
};

struct ReplacementResource
{
    ID3D12Resource* resource = nullptr;
    UINT64 availabilityFenceValue = 0;

    void destroy()
    {
        SafeRelease(resource);
        availabilityFenceValue = 0;
    }
};
} // namespace hxsc

// Async generation: records the copies of the game's inputs of frameID on a list of the game queue (run after the
// game's frame, before the generation queue's wait on it). False: nothing usable was recorded, the frame is not
// generated.
typedef bool (*HelixSRStageInputsFunc)(ID3D12GraphicsCommandList* gameList, uint64_t frameID, void* userContext);

// What the FG output sets for the next Present (applied at that Present, as AMD's FfxFrameGenerationConfig)
struct HelixSRSwapchainConfig
{
    bool frameGenerationEnabled = false;
    bool allowAsyncWorkloads = false;
    bool onlyPresentGenerated = false;
    uint32_t generatedFrames = 1; // frames generated per real frame, 1 .. hxsc::kMaxGeneratedFrames
    uint64_t frameID = 0;
    FfxApiRect2D generationRect {};
    FfxApiFrameGenerationDispatchFunc frameGenerationCallback = nullptr;
    void* frameGenerationCallbackUserContext = nullptr;
    HelixSRStageInputsFunc stageInputsCallback = nullptr; // needed for allowAsyncWorkloads
    void* stageInputsCallbackUserContext = nullptr;
};

struct HelixSRSwapchainPacingTuning
{
    float safetyMarginInMs = 0.1f;
    float varianceFactor = 0.1f;
    bool allowHybridSpin = false;
    uint32_t hybridSpinTime = 2;
    bool allowWaitForSingleObjectOnFence = false;
};

// {6C7E1C83-5B0A-4D52-9F5E-1E2C8A0B7D41}
static const GUID IID_IHelixSRSwapChain = { 0x6c7e1c83, 0x5b0a, 0x4d52, { 0x9f, 0x5e, 0x1e, 0x2c, 0x8a, 0x0b, 0x7d, 0x41 } };

class HelixSRSwapChainDX12 : public IDXGISwapChain4
{
  protected:
    HRESULT shutdown();
    UINT getInterpolationEnabledSwapChainFlags(UINT nonAdjustedFlags);
    DXGI_SWAP_CHAIN_DESC1 getInterpolationEnabledSwapChainDescription(const DXGI_SWAP_CHAIN_DESC1* nonAdjustedDesc);

    hxsc::PresentInfo presentInfo = {};
    HelixSRSwapchainConfig nextConfig = {};

    CRITICAL_SECTION criticalSection {};
    CRITICAL_SECTION criticalSectionUpdateConfig {};

    UINT gameBufferCount = 0;
    UINT gameFlags = 0;
    DXGI_SWAP_EFFECT gameSwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    UINT gameMaximumFrameLatency = 1;
    ID3D12Resource* realBackBuffer0 = nullptr; // held to avoid a DXGI deadlock on Windows 10

    HANDLE replacementFrameLatencyWaitableObjectHandle = 0;

    UINT64 interpolationFenceValue = 0;
    UINT64 gameFenceValue = 0;
    bool frameInterpolationResetCondition = false;
    FfxApiRect2D interpolationRect {};

    hxsc::ReplacementResource replacementSwapBuffers[hxsc::kMaxBufferCount] = {};
    hxsc::ReplacementResource interpolationOutputs[hxsc::kOutputSets][hxsc::kMaxGeneratedFrames] = {};
    UINT outputsCreated = 0; // images per set created so far (created when more generated frames are asked for)
    int replacementSwapBufferIndex = 0;
    UINT outputSet = 0;
    UINT64 presentCount = 0;

    bool tearingSupported = false;
    bool interpolationEnabled = false;
    bool presentInterpolatedOnly = false;
    uint32_t generatedFrames = 1;

    UINT64 currentFrameID = 0;

    UINT64 framesSentForPresentation = 0;
    UINT64 nextPresentWaitValue = 0;
    HANDLE interpolationThreadHandle = 0;
    ULONG refCount = 1;

    UINT backBufferTransferFunction = 0;
    float minLuminance = 0.0f;
    float maxLuminance = 0.0f;

    FfxApiFrameGenerationDispatchFunc frameGenerationCallback = nullptr;
    void* frameGenerationCallbackContext = nullptr;
    HelixSRStageInputsFunc stageInputsCallback = nullptr;
    void* stageInputsCallbackContext = nullptr;

    // refresh cap (VSync / fixed refresh): (generated + 1) x real frame rate stays at most the refresh rate
    double refreshRate = 0.0;      // Hz, 0 = unknown
    double realFrameSeconds = 0.0; // smoothed time between the game's Presents
    int64_t lastPresentQpc = 0;
    UINT refreshCap = 0;           // 0 = none yet
    UINT refreshCapFrames = 0;

    HRESULT presentImpl(UINT SyncInterval, UINT Flags, bool present1);
    void presentPassthrough(UINT SyncInterval, UINT Flags);
    HRESULT resizeImpl(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags,
                       bool resize1);

    bool stageInputs();
    UINT dispatchGeneration(UINT requested, ID3D12Resource* realFrame);
    void presentInterpolated(UINT SyncInterval, UINT Flags);

    bool verifyBackbufferDuplicateResources();
    bool ensureOutputs(UINT count);
    bool destroyReplacementResources();
    bool killPresenterThread();
    bool spawnPresenterThread();

    IDXGISwapChain4* real();

  public:
    void setConfig(const HelixSRSwapchainConfig& config);
    bool waitForPresents();
    void setWaitCallback(hxsc::WaitCallbackFunc waitCallbackFunc);
    void setFramePacingTuning(const HelixSRSwapchainPacingTuning& tuning);

    HelixSRSwapChainDX12();
    virtual ~HelixSRSwapChainDX12();

    HRESULT init(HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* swapChainDesc1,
                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, ID3D12CommandQueue* queue,
                 IDXGIFactory2* dxgiFactory);

    // Creates the swapchain for the game (as ffxCreateFrameinterpolationSwapchain*DX12)
    static HRESULT Create(IDXGIFactory* factory, ID3D12CommandQueue* queue, const DXGI_SWAP_CHAIN_DESC* desc,
                          IDXGISwapChain4** swapChain);
    static HRESULT CreateForHwnd(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hWnd,
                                 const DXGI_SWAP_CHAIN_DESC1* desc,
                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, IDXGISwapChain4** swapChain);

    // IUnknown
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject);
    virtual ULONG STDMETHODCALLTYPE AddRef();
    virtual ULONG STDMETHODCALLTYPE Release();

    // IDXGIObject
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData);
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown);
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData);
    virtual HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent);

    // IDXGIDeviceSubObject
    virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice);

    // IDXGISwapChain
    virtual HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags);
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface);
    virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget);
    virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget);
    virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc);
    virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                                    UINT SwapChainFlags);
    virtual HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters);
    virtual HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput);
    virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats);
    virtual HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount);

    // IDXGISwapChain1
    virtual HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc);
    virtual HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc);
    virtual HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd);
    virtual HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk);
    virtual HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
                                               const DXGI_PRESENT_PARAMETERS* pPresentParameters);
    virtual BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported(void);
    virtual HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput);
    virtual HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor);
    virtual HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor);
    virtual HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation);
    virtual HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation);

    // IDXGISwapChain2
    virtual HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height);
    virtual HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight);
    virtual HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency);
    virtual HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency);
    virtual HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject(void);
    virtual HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix);
    virtual HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix);

    // IDXGISwapChain3
    virtual UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(void);
    virtual HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                             UINT* pColorSpaceSupport);
    virtual HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace);
    virtual HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                                     UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                                     IUnknown* const* ppPresentQueue);

    // IDXGISwapChain4
    virtual HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData);
};
