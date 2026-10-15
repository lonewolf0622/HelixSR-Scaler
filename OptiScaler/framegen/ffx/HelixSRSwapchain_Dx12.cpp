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

// HelixSR Scaler: adapted from the FidelityFX SDK's FrameInterpolationSwapchainDX12.cpp and
// FrameInterpolationSwapchainDX12_Helpers.cpp (see HelixSRSwapchain_Dx12.h for what changed).
//
// Fences (values only grow, also across resizes and generation queue changes: no fence is ever moved back):
//   gameFence              game queue -> generation queue / present queue: the game's frame is rendered
//   interpolationFence     generation queue: this real frame's generation list (all N frames) finished; signaled
//                          once per real frame, also when the callback failed (after the wait on gameFence)
//   compositionFenceGPU    present queue: frame presentIndex was copied into the swapchain image (its output image
//                          can be written again)
//   compositionFenceCPU    presenter thread, right after queuing that copy (Present waits for it before it queues a
//                          wait on compositionFenceGPU, so no GPU wait is ever queued for a value nobody will signal)
//   presentFence           present queue: frame presentIndex was handed to DXGI
//   replacementBufferFence present queue: the real frame (the game's back buffer) was copied, the game may render
//                          into that buffer again
// presentIndex numbers every frame handed to the presenter: 1, 2, 3 ... across real and generated frames. For one
// real frame with N generated frames: base + 1 .. base + N are the generated ones, base + N + 1 the real one.

#include "pch.h"
#include "HelixSRSwapchain_Dx12.h"

#include <proxies/FfxApi_Proxy.h>
#include <ffx_antilag2_dx12.h>

#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

using namespace hxsc;

namespace
{
IDXGIFactory* getDXGIFactoryFromSwapChain(IDXGISwapChain* swapChain)
{
    IDXGIFactory* factory = nullptr;
    swapChain->GetParent(IID_PPV_ARGS(&factory));
    return factory;
}

void waitForPerformanceCount(const int64_t targetCount, const int64_t frequency, const UINT timerResolution,
                             const UINT spinTime)
{
    int64_t currentCount;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentCount));
    if (currentCount >= targetCount)
        return;

    double millis = static_cast<double>(((targetCount - currentCount) * 1000000) / frequency) / 1000.;

    // Sleep if safe, to free up cores
    while (timerResolution != kUnknownTimerResolution && millis > spinTime * timerResolution)
    {
        MMRESULT result = timeBeginPeriod(timerResolution);
        if (result != TIMERR_NOERROR)
            break;
        Sleep(static_cast<DWORD>((millis - timerResolution * spinTime)));
        timeEndPeriod(timerResolution);

        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentCount));
        millis = static_cast<double>(((targetCount - currentCount) * 1000000) / frequency) / 1000.;
    }

    do
    {
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentCount));
    } while (currentCount < targetCount);
}

bool waitForFenceValue(ID3D12Fence* fence, UINT64 value, DWORD dwMilliseconds = INFINITE,
                       WaitCallbackFunc waitCallback = nullptr, const bool waitForSingleObjectOnFence = false)
{
    bool status = false;

    if (fence)
    {
        int64_t originalQpc = 0;
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&originalQpc));
        int64_t currentQpc = originalQpc;
        int64_t qpcFrequency;
        QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&qpcFrequency));
        const DWORD waitCallbackIntervalInMs = 1;
        int64_t deltaQpcWaitCallback = qpcFrequency * waitCallbackIntervalInMs / 1000;
        int64_t deltaQpcTimeout = qpcFrequency * dwMilliseconds / 1000;
        wchar_t fenceName[64] {};
        uint32_t fenceNameLen = sizeof(fenceName);
        fence->GetPrivateData(WKPDID_D3DDebugObjectNameW, &fenceNameLen, &fenceName);

        if (waitForSingleObjectOnFence == false)
        {
            int64_t previousQpc = originalQpc;
            while (status != true)
            {
                status = fence->GetCompletedValue() >= value;
                QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentQpc));
                if (waitCallback)
                {
                    int64_t deltaQpc = currentQpc - previousQpc;
                    if (deltaQpc > deltaQpcWaitCallback)
                    {
                        waitCallback(fenceName, value);
                        previousQpc = currentQpc;
                    }
                }
                if (dwMilliseconds != INFINITE)
                {
                    int64_t deltaQpc = currentQpc - originalQpc;
                    if (deltaQpc > deltaQpcTimeout)
                        break;
                }
            }
        }
        else
        {
            status = fence->GetCompletedValue() >= value;

            if (!status)
            {
                HANDLE handle = CreateEvent(0, false, false, 0);

                if (handle != NULL)
                {
                    if (SUCCEEDED(fence->SetEventOnCompletion(value, handle)))
                    {
                        while (status != true)
                        {
                            QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentQpc));
                            int64_t deltaQpc = currentQpc - originalQpc;
                            if (deltaQpc > deltaQpcTimeout && dwMilliseconds != INFINITE)
                                break;
                            status = (WaitForSingleObject(handle, waitCallbackIntervalInMs) == WAIT_OBJECT_0);
                            if (waitCallback)
                                waitCallback(fenceName, value);
                        }
                    }
                    CloseHandle(handle);
                }
            }
        }
    }

    return status;
}

bool isTearingSupported(IDXGIFactory* dxgiFactory)
{
    BOOL bTearingSupported = FALSE;

    IDXGIFactory5* pFactory5 = nullptr;
    if (dxgiFactory && SUCCEEDED(dxgiFactory->QueryInterface(IID_PPV_ARGS(&pFactory5))))
    {
        pFactory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &bTearingSupported,
                                       sizeof(bTearingSupported));
        SafeRelease(pFactory5);
    }

    return bTearingSupported == TRUE;
}

bool isExclusiveFullscreen(IDXGISwapChain* swapChain)
{
    BOOL isFullscreen = FALSE;
    if (SUCCEEDED(swapChain->GetFullscreenState(&isFullscreen, nullptr)))
        return isFullscreen == TRUE;

    return false;
}

IDXGIOutput6* getMostRelevantOutputFromSwapChain(IDXGISwapChain* swapChain)
{
    IDXGIOutput6* pOutput6 = nullptr;

    IDXGIFactory* pFactory = getDXGIFactoryFromSwapChain(swapChain);
    if (pFactory)
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (SUCCEEDED(swapChain->GetDesc(&desc)))
        {
            UINT largestArea = 0;
            RECT windowRect {};
            if (GetWindowRect(desc.OutputWindow, &windowRect) == TRUE)
            {
                UINT adapterIdx = 0;
                IDXGIAdapter* pAdapter = nullptr;
                while (pFactory->EnumAdapters(adapterIdx++, &pAdapter) != DXGI_ERROR_NOT_FOUND)
                {
                    UINT outputIdx = 0;
                    IDXGIOutput* pOutput = nullptr;
                    while (pAdapter->EnumOutputs(outputIdx++, &pOutput) != DXGI_ERROR_NOT_FOUND)
                    {
                        DXGI_OUTPUT_DESC outputDesc {};
                        if (SUCCEEDED(pOutput->GetDesc(&outputDesc)))
                        {
                            RECT intersection {};
                            if (IntersectRect(&intersection, &windowRect, &outputDesc.DesktopCoordinates) == TRUE)
                            {
                                UINT area = (intersection.right - intersection.left) *
                                            (intersection.bottom - intersection.top);

                                if (area > largestArea)
                                {
                                    SafeRelease(pOutput6);
                                    if (SUCCEEDED(pOutput->QueryInterface(IID_PPV_ARGS(&pOutput6))))
                                        largestArea = area;
                                }
                            }
                        }

                        SafeRelease(pOutput);
                    }

                    SafeRelease(pAdapter);
                }
            }
        }
        SafeRelease(pFactory);
    }

    return pOutput6;
}

bool getMonitorLuminanceRange(IDXGISwapChain* swapChain, float* outMinLuminance, float* outMaxLuminance)
{
    bool bResult = false;

    IDXGIOutput6* dxgiOutput = getMostRelevantOutputFromSwapChain(swapChain);
    if (dxgiOutput)
    {
        DXGI_OUTPUT_DESC1 outputDesc1;
        if (SUCCEEDED(dxgiOutput->GetDesc1(&outputDesc1)))
        {
            *outMinLuminance = outputDesc1.MinLuminance;
            *outMaxLuminance = outputDesc1.MaxLuminance;
            bResult = true;
        }
        SafeRelease(dxgiOutput);
    }

    return bResult;
}

// The refresh rate of the monitor the window is on (0: unknown)
double getRefreshRate(IDXGISwapChain* swapChain)
{
    double hz = 0.0;

    IDXGIOutput6* dxgiOutput = getMostRelevantOutputFromSwapChain(swapChain);
    if (dxgiOutput)
    {
        DXGI_OUTPUT_DESC desc {};
        if (SUCCEEDED(dxgiOutput->GetDesc(&desc)))
        {
            MONITORINFOEXW info {};
            info.cbSize = sizeof(info);
            DEVMODEW mode {};
            mode.dmSize = sizeof(mode);
            if (GetMonitorInfoW(desc.Monitor, &info) &&
                EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
            {
                hz = static_cast<double>(mode.dmDisplayFrequency);
            }
        }
        SafeRelease(dxgiOutput);
    }

    return hz;
}

// The image of one present copied into the swapchain's image (no UI composition: HelixSR's generated frames have the
// game's UI, the real frame is the game's back buffer)
void copyToSwapchain(ID3D12GraphicsCommandList* list, ID3D12Resource* src, D3D12_RESOURCE_STATES srcState,
                     ID3D12Resource* dst)
{
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    UINT count = 0;

    if (srcState != D3D12_RESOURCE_STATE_COPY_SOURCE)
    {
        barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[count].Transition.pResource = src;
        barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[count].Transition.StateBefore = srcState;
        barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        count++;
    }

    barriers[count].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[count].Transition.pResource = dst;
    barriers[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[count].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[count].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    count++;

    list->ResourceBarrier(count, barriers);
    list->CopyResource(dst, src);

    for (UINT i = 0; i < count; i++)
        std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);

    list->ResourceBarrier(count, barriers);
}

// Copies one present's image into the current swapchain image on the present queue, then marks presentIndex
// composited (GPU and CPU fence)
void compositeSwapChainFrame(PresentInfo* presenter, PacingData* entry, uint32_t frameType)
{
    const PacingData::FrameInfo& frameInfo = entry->frames[frameType];

    // the generation list of this real frame finished (covers the generated images and, through its wait on the
    // game fence, the game's frame)
    presenter->presentQueue->Wait(presenter->interpolationFence, frameInfo.interpolationCompletedFenceValue);

    auto gpuCommands = presenter->commandPool.get(presenter->presentQueue, L"HelixSR compositeSwapChainFrame");
    if (gpuCommands != nullptr)
    {
        uint32_t currBackbufferIndex = presenter->swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* swapchainBackbuffer = nullptr;
        presenter->swapChain->GetBuffer(currBackbufferIndex, IID_PPV_ARGS(&swapchainBackbuffer));

        if (swapchainBackbuffer != nullptr && frameInfo.resource != nullptr)
            copyToSwapchain(gpuCommands->reset(), frameInfo.resource, frameInfo.state, swapchainBackbuffer);
        else
            gpuCommands->reset();

        gpuCommands->execute(true);
        SafeRelease(swapchainBackbuffer);
    }
    else
    {
        LOG_ERROR("HelixSR swapchain: no free command list for the present copy");
    }

    // always signaled, also without a copy: waits on these values are queued for every scheduled present
    presenter->presentQueue->Signal(presenter->compositionFenceGPU, frameInfo.presentIndex);
    presenter->compositionFenceCPU->Signal(frameInfo.presentIndex);
}

void presentToSwapChain(PresentInfo* presenter, PacingData* entry, PacingData::FrameType frameType)
{
    const PacingData::FrameInfo& frameInfo = entry->frames[frameType];

    const UINT uSyncInterval = entry->vsync ? 1 : 0;
    const bool bExclusiveFullscreen = isExclusiveFullscreen(presenter->swapChain);
    const bool bSetAllowTearingFlag = entry->tearingSupported && !bExclusiveFullscreen && (0 == uSyncInterval);
    const UINT uFlags = bSetAllowTearingFlag * DXGI_PRESENT_ALLOW_TEARING;

    struct AntiLag2Data
    {
        AMD::AntiLag2DX12::Context* context;
        bool enabled;
    } data;

    static const GUID IID_IFfxAntiLag2Data = { 0x5083ae5b, 0x8070, 0x4fca, { 0x8e, 0xe5, 0x35, 0x82, 0xdd, 0x36, 0x7d, 0x13 } };

    const bool isGenerated = frameType != PacingData::Real;

    UINT size = sizeof(data);
    if (SUCCEEDED(presenter->swapChain->GetPrivateData(IID_IFfxAntiLag2Data, &size, &data)) && size == sizeof(data))
    {
        if (data.enabled && data.context != nullptr)
            AMD::AntiLag2DX12::SetFrameGenFrameType(data.context, isGenerated);
    }

    presenter->swapChain->Present(uSyncInterval, uFlags);

    // frames sent for presentation
    presenter->presentQueue->Signal(presenter->presentFence, frameInfo.presentIndex);
}

DWORD WINAPI presenterThread(LPVOID param)
{
    PresentInfo* presenter = static_cast<PresentInfo*>(param);

    if (presenter)
    {
        UINT64 numFramesSentForPresentation = 0;
        int64_t qpcFrequency = 0;

        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpcFrequency = freq.QuadPart;

        TIMECAPS timerCaps;
        timerCaps.wPeriodMin = kUnknownTimerResolution;

        presenter->previousPresentQpc = 0;

        while (!presenter->shutdown)
        {
            WaitForSingleObject(presenter->pacerEvent, INFINITE);

            if (!presenter->shutdown)
            {
                EnterCriticalSection(&presenter->criticalSectionScheduledFrame);

                PacingData entry = presenter->scheduledPresents;
                presenter->scheduledPresents.invalidate();

                LeaveCriticalSection(&presenter->criticalSectionScheduledFrame);

                if (entry.numFramesToPresent > 0)
                {
                    // an entry may have been replaced before it was taken: move the present fence up to this entry's
                    // base, otherwise a wait for a dropped entry's presents would never end
                    presenter->presentQueue->Signal(presenter->presentFence, entry.numFramesSentForPresentationBase);
                    presenter->presentQueue->Wait(presenter->interpolationFence,
                                                  entry.interpolationCompletedFenceValue);

                    // Generated_1 .. Generated_N, then Real (presentIndex grows in this order)
                    for (uint32_t frameType = 0; frameType < PacingData::FrameType::Count; frameType++)
                    {
                        const PacingData::FrameInfo& frameInfo = entry.frames[frameType];
                        if (frameInfo.doPresent)
                        {
                            compositeSwapChainFrame(presenter, &entry, frameType);

                            // the game's back buffer can be rendered into again
                            if (frameInfo.presentIndex == entry.replacementBufferFenceSignal)
                            {
                                presenter->presentQueue->Signal(presenter->replacementBufferFence,
                                                                entry.replacementBufferFenceSignal);
                            }

                            MMRESULT result = timeGetDevCaps(&timerCaps, sizeof(timerCaps));
                            if (result != MMSYSERR_NOERROR || !presenter->allowHybridSpin)
                                timerCaps.wPeriodMin = kUnknownTimerResolution;
                            else
                                timerCaps.wPeriodMin = (std::max)(1u, timerCaps.wPeriodMin);

                            // pacing
                            waitForFenceValue(presenter->compositionFenceGPU, frameInfo.presentIndex);
                            int64_t targetQpc = presenter->previousPresentQpc + frameInfo.presentQpcDelta;
                            waitForPerformanceCount(targetQpc, qpcFrequency, timerCaps.wPeriodMin,
                                                    presenter->hybridSpinTime);

                            int64_t currentPresentQPC;
                            QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentPresentQPC));
                            presenter->previousPresentQpc = currentPresentQPC;

                            presentToSwapChain(presenter, &entry, (PacingData::FrameType) frameType);
                        }
                    }

                    numFramesSentForPresentation = entry.numFramesSentForPresentationBase + entry.numFramesToPresent;
                }
            }
        }

        waitForFenceValue(presenter->presentFence, numFramesSentForPresentation);
    }

    return 0;
}

DWORD WINAPI interpolationThread(LPVOID param)
{
    PresentInfo* presenter = static_cast<PresentInfo*>(param);

    if (presenter)
    {
        HANDLE presenterThreadHandle = CreateThread(nullptr, 0, presenterThread, param, 0, nullptr);

        if (presenterThreadHandle != 0)
        {
            SetThreadPriority(presenterThreadHandle, THREAD_PRIORITY_HIGHEST);
            SetThreadDescription(presenterThreadHandle, L"HelixSR Presenter Thread");

            SimpleMovingAverage<10, double> frameTime {};

            int64_t previousQpc = 0;
            int64_t previousDelta = 0;
            int64_t qpcFrequency;
            QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&qpcFrequency));

            while (!presenter->shutdown)
            {
                WaitForSingleObject(presenter->presentEvent, INFINITE);

                if (!presenter->shutdown)
                {
                    EnterCriticalSection(&presenter->criticalSectionScheduledFrame);

                    PacingData entry = presenter->scheduledInterpolations;
                    presenter->scheduledInterpolations.invalidate();

                    LeaveCriticalSection(&presenter->criticalSectionScheduledFrame);

                    int64_t preWaitQPC = 0;
                    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&preWaitQPC));
                    int64_t previousPresentQPC = presenter->previousPresentQpc;
                    int64_t targetDelta = (previousPresentQPC + previousDelta) - preWaitQPC;

                    // the generation list of this real frame (all its generated frames)
                    const bool allowSleep =
                        previousPresentQPC && (targetDelta * 1000000) / qpcFrequency > 2000 &&
                        presenter->allowWaitForSingleObjectOnFence;
                    waitForFenceValue(presenter->interpolationFence, entry.interpolationCompletedFenceValue, INFINITE,
                                      nullptr, allowSleep);

                    SetEvent(presenter->interpolationEvent);

                    int64_t currentQpc = 0;
                    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentQpc));

                    const double deltaQpc = double(currentQpc - previousQpc) * (previousQpc > 0);
                    previousQpc = currentQpc;

                    // reset pacing averaging if delta > 10 fps
                    const float fTimeoutInSeconds = 0.1f;
                    double deltaQpcResetThreshold = double(qpcFrequency * fTimeoutInSeconds);
                    if ((deltaQpc > deltaQpcResetThreshold) || presenter->resetTimer)
                        frameTime.reset();
                    else
                        frameTime.update(deltaQpc);

                    // the real frame time shared by this real frame's presents (N generated + the real one); reduced
                    // by the variance (as AMD's 2x: average / 2 - variance * factor per present) and a safety margin,
                    // so pacing never settles below the game's frame rate
                    const UINT presents = (std::max)(1u, (UINT) entry.numFramesToPresent);
                    int64_t qpcSafetyMargin = int64_t(qpcFrequency * presenter->safetyMarginInSec);
                    const int64_t conservativeAvg =
                        int64_t((frameTime.getAverage() - 2.0 * frameTime.getVariance() * presenter->varianceFactor) /
                                presents);
                    const int64_t deltaToUse =
                        conservativeAvg > qpcSafetyMargin ? (conservativeAvg - qpcSafetyMargin) : 0;

                    for (auto& frame : entry.frames)
                        frame.presentQpcDelta = deltaToUse;

                    previousDelta = deltaToUse;

                    // schedule presents
                    EnterCriticalSection(&presenter->criticalSectionScheduledFrame);
                    presenter->scheduledPresents = entry;
                    LeaveCriticalSection(&presenter->criticalSectionScheduledFrame);
                    SetEvent(presenter->pacerEvent);
                }
            }

            // signal event to allow thread to finish
            SetEvent(presenter->pacerEvent);
            WaitForSingleObject(presenterThreadHandle, INFINITE);
            SafeCloseHandle(presenterThreadHandle);
        }
    }

    return 0;
}
} // namespace

// --- creation ------------------------------------------------------------------------------------------------------

HRESULT HelixSRSwapChainDX12::CreateForHwnd(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hWnd,
                                            const DXGI_SWAP_CHAIN_DESC1* desc1,
                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                            IDXGISwapChain4** swapChain)
{
    if (factory == nullptr || queue == nullptr || desc1 == nullptr || hWnd == 0 || swapChain == nullptr)
        return E_INVALIDARG;

    HRESULT hr = E_INVALIDARG;

    IDXGIFactory2* dxgiFactory2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&dxgiFactory2))))
    {
        auto* sc = new HelixSRSwapChainDX12();
        hr = sc->init(hWnd, desc1, fullscreenDesc, queue, dxgiFactory2);

        if (SUCCEEDED(hr))
            *swapChain = sc;
        else
            delete sc;

        SafeRelease(dxgiFactory2);
    }

    return hr;
}

HRESULT HelixSRSwapChainDX12::Create(IDXGIFactory* factory, ID3D12CommandQueue* queue, const DXGI_SWAP_CHAIN_DESC* desc,
                                     IDXGISwapChain4** swapChain)
{
    if (desc == nullptr)
        return E_INVALIDARG;

    DXGI_SWAP_CHAIN_DESC1 desc1 {};
    desc1.Width = desc->BufferDesc.Width;
    desc1.Height = desc->BufferDesc.Height;
    desc1.Format = desc->BufferDesc.Format;
    desc1.SampleDesc = desc->SampleDesc;
    desc1.BufferUsage = desc->BufferUsage;
    desc1.BufferCount = desc->BufferCount;
    desc1.SwapEffect = desc->SwapEffect;
    desc1.Flags = desc->Flags;
    desc1.Scaling = DXGI_SCALING_STRETCH;
    desc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc1.Stereo = FALSE;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc {};
    fullscreenDesc.Scaling = desc->BufferDesc.Scaling;
    fullscreenDesc.RefreshRate = desc->BufferDesc.RefreshRate;
    fullscreenDesc.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fullscreenDesc.Windowed = desc->Windowed;

    return CreateForHwnd(factory, queue, desc->OutputWindow, &desc1, &fullscreenDesc, swapChain);
}

bool HelixSRSwapChainDX12::verifyBackbufferDuplicateResources()
{
    HRESULT hr = S_OK;

    ID3D12Device* device = nullptr;
    ID3D12Resource* buffer = nullptr;
    if (SUCCEEDED(real()->GetBuffer(0, IID_PPV_ARGS(&buffer))))
    {
        if (SUCCEEDED(buffer->GetDevice(IID_PPV_ARGS(&device))))
        {
            auto bufferDesc = buffer->GetDesc();
            D3D12_CLEAR_VALUE clearValue { bufferDesc.Format, { 0.f, 0.f, 0.f, 1.f } };

            D3D12_HEAP_PROPERTIES heapProperties {};
            D3D12_HEAP_FLAGS heapFlags;
            buffer->GetHeapProperties(&heapProperties, &heapFlags);

            heapFlags &= ~D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
            heapFlags &= ~D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
            heapFlags &= ~D3D12_HEAP_FLAG_DENY_BUFFERS;
            heapFlags &= ~D3D12_HEAP_FLAG_ALLOW_DISPLAY;

            for (size_t i = 0; i < gameBufferCount; i++)
            {
                if (replacementSwapBuffers[i].resource == nullptr)
                {
                    if (FAILED(device->CreateCommittedResource(&heapProperties, heapFlags, &bufferDesc,
                                                               D3D12_RESOURCE_STATE_PRESENT, &clearValue,
                                                               IID_PPV_ARGS(&replacementSwapBuffers[i].resource))))
                    {
                        hr |= E_FAIL;
                    }
                    else
                    {
                        replacementSwapBuffers[i].resource->SetName(L"HelixSR Replacement BackBuffer");
                    }
                }
            }

            SafeRelease(device);
        }

        SafeRelease(realBackBuffer0);
        realBackBuffer0 = buffer;
    }

    return SUCCEEDED(hr);
}

// The generated frames' images: count per set, both sets (created once, kept until the next resize)
bool HelixSRSwapChainDX12::ensureOutputs(UINT count)
{
    count = (std::min)(count, kMaxGeneratedFrames);

    if (outputsCreated >= count)
        return true;

    if (realBackBuffer0 == nullptr && !verifyBackbufferDuplicateResources())
        return false;

    if (realBackBuffer0 == nullptr)
        return false;

    ID3D12Device* device = nullptr;
    if (FAILED(realBackBuffer0->GetDevice(IID_PPV_ARGS(&device))))
        return false;

    auto bufferDesc = realBackBuffer0->GetDesc();
    bufferDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_CLEAR_VALUE clearValue { bufferDesc.Format, { 0.f, 0.f, 0.f, 1.f } };

    D3D12_HEAP_PROPERTIES heapProperties {};
    D3D12_HEAP_FLAGS heapFlags;
    realBackBuffer0->GetHeapProperties(&heapProperties, &heapFlags);

    heapFlags &= ~D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
    heapFlags &= ~D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
    heapFlags &= ~D3D12_HEAP_FLAG_DENY_BUFFERS;
    heapFlags &= ~D3D12_HEAP_FLAG_ALLOW_DISPLAY;

    bool ok = true;
    for (UINT i = outputsCreated; i < count && ok; i++)
    {
        for (UINT set = 0; set < kOutputSets && ok; set++)
        {
            auto& out = interpolationOutputs[set][i];
            if (out.resource != nullptr)
                continue;

            // a new image: nothing waits for it (availability 0)
            out.availabilityFenceValue = 0;
            if (FAILED(device->CreateCommittedResource(&heapProperties, heapFlags, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &clearValue,
                                                       IID_PPV_ARGS(&out.resource))))
            {
                out.resource = nullptr;
                ok = false;
            }
            else
            {
                out.resource->SetName(L"HelixSR Generated Frame");
            }
        }

        if (ok)
            outputsCreated = i + 1;
    }

    SafeRelease(device);

    if (!ok)
        LOG_ERROR("HelixSR swapchain: generated frame images not created ({} of {} per set)", outputsCreated, count);

    return outputsCreated >= count;
}

HRESULT HelixSRSwapChainDX12::init(HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, ID3D12CommandQueue* queue,
                                   IDXGIFactory2* dxgiFactory)
{
    gameBufferCount = desc->BufferCount;
    gameFlags = desc->Flags;
    gameSwapEffect = desc->SwapEffect;

    HRESULT hr = E_FAIL;

    if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&presentInfo.device))))
    {
        presentInfo.gameQueue = queue;

        InitializeCriticalSection(&criticalSection);
        InitializeCriticalSection(&criticalSectionUpdateConfig);
        InitializeCriticalSection(&presentInfo.criticalSectionScheduledFrame);
        presentInfo.presentEvent = CreateEvent(NULL, FALSE, FALSE, nullptr);
        presentInfo.interpolationEvent = CreateEvent(NULL, FALSE, TRUE, nullptr);
        presentInfo.pacerEvent = CreateEvent(NULL, FALSE, FALSE, nullptr);
        tearingSupported = isTearingSupported(dxgiFactory);

        // presentation queue
        D3D12_COMMAND_QUEUE_DESC presentQueueDesc = queue->GetDesc();
        presentQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        presentQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        presentQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        presentQueueDesc.NodeMask = 0;
        if (FAILED(presentInfo.device->CreateCommandQueue(&presentQueueDesc, IID_PPV_ARGS(&presentInfo.presentQueue))))
            return E_FAIL;
        presentInfo.presentQueue->SetName(L"HelixSR PresentQueue");

        IDXGISwapChain1* pSwapChain1 = nullptr;

        DXGI_SWAP_CHAIN_DESC1 realDesc = getInterpolationEnabledSwapChainDescription(desc);
        hr = dxgiFactory->CreateSwapChainForHwnd(presentInfo.presentQueue, hWnd, &realDesc, fullscreenDesc, nullptr,
                                                 &pSwapChain1);
        if (SUCCEEDED(hr))
        {
            hr = pSwapChain1->QueryInterface(IID_PPV_ARGS(&presentInfo.swapChain));
            SafeRelease(pSwapChain1);

            if (FAILED(hr))
                return hr;
        }
        else
        {
            return hr;
        }

        getMonitorLuminanceRange(presentInfo.swapChain, &minLuminance, &maxLuminance);

        presentInfo.device->CreateFence(gameFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.gameFence));
        presentInfo.gameFence->SetName(L"HelixSR GameFence");

        presentInfo.device->CreateFence(interpolationFenceValue, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&presentInfo.interpolationFence));
        presentInfo.interpolationFence->SetName(L"HelixSR InterpolationFence");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&presentInfo.presentFence));
        presentInfo.presentFence->SetName(L"HelixSR PresentFence");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&presentInfo.replacementBufferFence));
        presentInfo.replacementBufferFence->SetName(L"HelixSR ReplacementBufferFence");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&presentInfo.compositionFenceGPU));
        presentInfo.compositionFenceGPU->SetName(L"HelixSR CompositionFence GPU");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&presentInfo.compositionFenceCPU));
        presentInfo.compositionFenceCPU->SetName(L"HelixSR CompositionFence CPU");

        if ((desc->Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) ==
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
        {
            replacementFrameLatencyWaitableObjectHandle = CreateEvent(0, FALSE, TRUE, nullptr);
        }

        // async compute queue for the generation (allowAsyncWorkloads)
        D3D12_COMMAND_QUEUE_DESC queueDesc = queue->GetDesc();
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        queueDesc.NodeMask = 0;
        presentInfo.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&presentInfo.asyncComputeQueue));
        if (presentInfo.asyncComputeQueue)
            presentInfo.asyncComputeQueue->SetName(L"HelixSR AsyncComputeQueue");

        // generation on the game queue by default
        presentInfo.interpolationQueue = presentInfo.gameQueue;

        refreshRate = getRefreshRate(presentInfo.swapChain);
        LOG_INFO("HelixSR swapchain created: {} buffers, tearing {}, refresh {} Hz", gameBufferCount,
                 tearingSupported, refreshRate);
    }

    return hr;
}

HelixSRSwapChainDX12::HelixSRSwapChainDX12() {}

HelixSRSwapChainDX12::~HelixSRSwapChainDX12() { shutdown(); }

UINT HelixSRSwapChainDX12::getInterpolationEnabledSwapChainFlags(UINT nonAdjustedFlags)
{
    UINT flags = nonAdjustedFlags;

    // the flag changes the runtime's fence behavior: our own waitable object is given to the game, the flag stays
    flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    if (tearingSupported)
        flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    return flags;
}

DXGI_SWAP_CHAIN_DESC1 HelixSRSwapChainDX12::getInterpolationEnabledSwapChainDescription(
    const DXGI_SWAP_CHAIN_DESC1* nonAdjustedDesc)
{
    DXGI_SWAP_CHAIN_DESC1 fiDesc = *nonAdjustedDesc;

    fiDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    fiDesc.BufferCount = 3;
    fiDesc.Flags = getInterpolationEnabledSwapChainFlags(fiDesc.Flags);

    return fiDesc;
}

IDXGISwapChain4* HelixSRSwapChainDX12::real() { return presentInfo.swapChain; }

HRESULT HelixSRSwapChainDX12::shutdown()
{
    if (presentInfo.device)
    {
        destroyReplacementResources();

        EnterCriticalSection(&criticalSection);
        killPresenterThread();
        LeaveCriticalSection(&criticalSection);

        SafeCloseHandle(presentInfo.presentEvent);
        SafeCloseHandle(presentInfo.interpolationEvent);
        SafeCloseHandle(presentInfo.pacerEvent);

        if (presentInfo.interpolationQueue && presentInfo.interpolationFence)
        {
            presentInfo.interpolationQueue->Signal(presentInfo.interpolationFence, ++interpolationFenceValue);
            waitForFenceValue(presentInfo.interpolationFence, interpolationFenceValue, INFINITE,
                              presentInfo.waitCallback);
        }

        SafeRelease(presentInfo.asyncComputeQueue);
        SafeRelease(presentInfo.presentQueue);

        SafeRelease(presentInfo.interpolationFence);
        SafeRelease(presentInfo.presentFence);
        SafeRelease(presentInfo.replacementBufferFence);
        SafeRelease(presentInfo.compositionFenceGPU);
        SafeRelease(presentInfo.compositionFenceCPU);

        SafeRelease(presentInfo.swapChain);

        if (presentInfo.gameFence)
            waitForFenceValue(presentInfo.gameFence, gameFenceValue, INFINITE, presentInfo.waitCallback);
        SafeRelease(presentInfo.gameFence);

        SafeCloseHandle(replacementFrameLatencyWaitableObjectHandle);

        DeleteCriticalSection(&criticalSection);
        DeleteCriticalSection(&criticalSectionUpdateConfig);
        DeleteCriticalSection(&presentInfo.criticalSectionScheduledFrame);

        SafeRelease(presentInfo.device);
    }

    return S_OK;
}

bool HelixSRSwapChainDX12::killPresenterThread()
{
    if (interpolationThreadHandle != NULL)
    {
        presentInfo.shutdown = true;

        SetEvent(presentInfo.presentEvent);
        WaitForSingleObject(interpolationThreadHandle, INFINITE);
        SafeCloseHandle(interpolationThreadHandle);
    }

    return interpolationThreadHandle == nullptr;
}

bool HelixSRSwapChainDX12::spawnPresenterThread()
{
    if (interpolationThreadHandle == NULL)
    {
        presentInfo.shutdown = false;
        interpolationThreadHandle =
            CreateThread(nullptr, 0, interpolationThread, reinterpret_cast<void*>(&presentInfo), 0, nullptr);

        if (interpolationThreadHandle != 0)
        {
            SetThreadPriority(interpolationThreadHandle, THREAD_PRIORITY_HIGHEST);
            SetThreadDescription(interpolationThreadHandle, L"HelixSR Interpolation Thread");
        }

        SetEvent(presentInfo.interpolationEvent);
    }

    return interpolationThreadHandle != NULL;
}

// --- configuration -------------------------------------------------------------------------------------------------

void HelixSRSwapChainDX12::setConfig(const HelixSRSwapchainConfig& config)
{
    EnterCriticalSection(&criticalSectionUpdateConfig);

    // Present passes the internal copy to apply it
    bool applyChangesNow = (&config == &nextConfig);

    ID3D12CommandQueue* inputInterpolationQueue =
        (config.allowAsyncWorkloads && presentInfo.asyncComputeQueue != nullptr) ? presentInfo.asyncComputeQueue
                                                                                  : presentInfo.gameQueue;

    if (!applyChangesNow)
    {
        nextConfig = config;

        if (presentInfo.interpolationQueue != inputInterpolationQueue ||
            interpolationEnabled != config.frameGenerationEnabled ||
            frameGenerationCallback != config.frameGenerationCallback ||
            frameGenerationCallbackContext != config.frameGenerationCallbackUserContext)
        {
            applyChangesNow = true;
        }
    }

    if (applyChangesNow)
    {
        EnterCriticalSection(&criticalSection);

        currentFrameID = config.frameID;
        presentInterpolatedOnly = config.onlyPresentGenerated;
        interpolationRect = config.generationRect;
        generatedFrames = std::clamp(config.generatedFrames, 1u, kMaxGeneratedFrames);

        if (presentInfo.interpolationQueue != inputInterpolationQueue)
        {
            waitForPresents();

            // new generation queue: everything on the old one completed (waitForPresents); the interpolation fence
            // keeps its value (never moved back, see destroyReplacementResources) and the new queue signals the next
            presentInfo.interpolationQueue = inputInterpolationQueue;
        }

        if (interpolationEnabled != config.frameGenerationEnabled ||
            frameGenerationCallback != config.frameGenerationCallback ||
            frameGenerationCallbackContext != config.frameGenerationCallbackUserContext)
        {
            waitForPresents();
            frameGenerationCallback = config.frameGenerationCallback;
            frameGenerationCallbackContext = config.frameGenerationCallbackUserContext;

            if (interpolationEnabled != config.frameGenerationEnabled)
            {
                interpolationEnabled = config.frameGenerationEnabled;
                if (interpolationEnabled)
                {
                    frameInterpolationResetCondition = true;
                    nextPresentWaitValue = framesSentForPresentation;

                    spawnPresenterThread();
                }
                else
                {
                    killPresenterThread();
                }
            }
        }

        LeaveCriticalSection(&criticalSection);
    }

    LeaveCriticalSection(&criticalSectionUpdateConfig);
}

bool HelixSRSwapChainDX12::destroyReplacementResources()
{
    EnterCriticalSection(&criticalSection);

    waitForPresents();

    const bool recreatePresenterThread = interpolationThreadHandle != nullptr;
    if (recreatePresenterThread)
        killPresenterThread();

    for (size_t i = 0; i < _countof(replacementSwapBuffers); i++)
        replacementSwapBuffers[i].destroy();

    SafeRelease(realBackBuffer0);

    for (auto& set : interpolationOutputs)
    {
        for (auto& out : set)
            out.destroy();
    }
    outputsCreated = 0;

    // Buffer indices start again. Unlike AMD's version the fence values are NOT set back to 0: a wait queued on a GPU
    // queue (e.g. the game queue's wait on the present fence) may not have been processed yet even though the fence
    // already passed its value, and a fence moved back below it would block that queue forever. The counters keep
    // growing; the new images have availability 0, which every fence has passed.
    replacementSwapBufferIndex = 0;
    presentCount = 0;
    outputSet = 0;

    frameInterpolationResetCondition = true;

    if (recreatePresenterThread)
        spawnPresenterThread();

    LeaveCriticalSection(&criticalSection);

    return true;
}

bool HelixSRSwapChainDX12::waitForPresents()
{
    waitForFenceValue(presentInfo.gameFence, gameFenceValue, INFINITE, presentInfo.waitCallback);
    waitForFenceValue(presentInfo.interpolationFence, interpolationFenceValue, INFINITE, presentInfo.waitCallback);
    waitForFenceValue(presentInfo.presentFence, framesSentForPresentation, INFINITE, presentInfo.waitCallback);

    return true;
}

void HelixSRSwapChainDX12::setWaitCallback(WaitCallbackFunc waitCallbackFunc) { presentInfo.waitCallback = waitCallbackFunc; }

void HelixSRSwapChainDX12::setFramePacingTuning(const HelixSRSwapchainPacingTuning& tuning)
{
    presentInfo.safetyMarginInSec = static_cast<double>(tuning.safetyMarginInMs) / 1000.0;
    presentInfo.varianceFactor = static_cast<double>(tuning.varianceFactor);
    presentInfo.allowHybridSpin = tuning.allowHybridSpin;
    presentInfo.hybridSpinTime = tuning.hybridSpinTime;
    presentInfo.allowWaitForSingleObjectOnFence = tuning.allowWaitForSingleObjectOnFence;
}

// --- IUnknown ------------------------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::QueryInterface(REFIID riid, void** ppvObject)
{
    const GUID guidReplacements[] = { IID_IUnknown,        IID_IDXGIObject,     IID_IDXGIDeviceSubObject,
                                      IID_IDXGISwapChain,  IID_IDXGISwapChain1, IID_IDXGISwapChain2,
                                      IID_IDXGISwapChain3, IID_IDXGISwapChain4, IID_IHelixSRSwapChain };

    for (auto guid : guidReplacements)
    {
        if (IsEqualGUID(riid, guid) == TRUE)
        {
            AddRef();
            *ppvObject = this;
            return S_OK;
        }
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE HelixSRSwapChainDX12::AddRef() { return InterlockedIncrement(&refCount); }

ULONG STDMETHODCALLTYPE HelixSRSwapChainDX12::Release()
{
    ULONG ref = InterlockedDecrement(&refCount);
    if (ref != 0)
        return ref;

    delete this;
    return 0;
}

// --- IDXGIObject ---------------------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return real()->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return real()->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return real()->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetParent(REFIID riid, void** ppParent)
{
    return real()->GetParent(riid, ppParent);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetDevice(REFIID riid, void** ppDevice)
{
    return real()->GetDevice(riid, ppDevice);
}

// --- presenting ----------------------------------------------------------------------------------------------------

// Frame generation off: the game's frame copied into the swapchain image and presented right away
void HelixSRSwapChainDX12::presentPassthrough(UINT SyncInterval, UINT Flags)
{
    const UINT currentBackBufferIndex = (UINT) replacementSwapBufferIndex;

    // the present queue waits for the game's frame
    presentInfo.gameQueue->Signal(presentInfo.gameFence, ++gameFenceValue);
    presentInfo.presentQueue->Wait(presentInfo.gameFence, gameFenceValue);

    auto commands = presentInfo.commandPool.get(presentInfo.presentQueue, L"HelixSR passthrough copy");
    if (commands != nullptr)
    {
        auto list = commands->reset();

        ID3D12Resource* dx12SwapchainBuffer = nullptr;
        UINT realIndex = presentInfo.swapChain->GetCurrentBackBufferIndex();
        presentInfo.swapChain->GetBuffer(realIndex, IID_PPV_ARGS(&dx12SwapchainBuffer));

        if (dx12SwapchainBuffer != nullptr && replacementSwapBuffers[currentBackBufferIndex].resource != nullptr)
        {
            copyToSwapchain(list, replacementSwapBuffers[currentBackBufferIndex].resource, D3D12_RESOURCE_STATE_PRESENT,
                            dx12SwapchainBuffer);
        }

        commands->execute(true);
        SafeRelease(dx12SwapchainBuffer);
    }

    presentInfo.presentQueue->Signal(presentInfo.replacementBufferFence, ++framesSentForPresentation);
    presentInfo.presentQueue->Signal(presentInfo.compositionFenceGPU, framesSentForPresentation);
    presentInfo.compositionFenceCPU->Signal(framesSentForPresentation);

    presentInfo.swapChain->Present(SyncInterval, Flags);

    presentInfo.presentQueue->Signal(presentInfo.presentFence, framesSentForPresentation);
    presentInfo.gameQueue->Wait(presentInfo.presentFence, framesSentForPresentation);
}

// Records the frame generation of this real frame: one command list on the generation queue for all requested
// generated frames (outputs[0 .. requested - 1] of the current set). Returns the frames generated (0: none, show
// only the real frame). The interpolation fence is signaled once, whatever happened.
UINT HelixSRSwapChainDX12::dispatchGeneration(UINT requested, ID3D12Resource* realFrame)
{
    UINT generated = 0;
    UINT count = std::clamp(requested, 1u, kMaxGeneratedFrames);

    if (!ensureOutputs(count))
        count = outputsCreated;

    if (count > 0 && realFrame != nullptr)
    {
        // the images of this set are written again only after the presenter copied them for their last present
        UINT64 freeValue = 0;
        for (UINT i = 0; i < count; i++)
            freeValue = (std::max)(freeValue, interpolationOutputs[outputSet][i].availabilityFenceValue);

        presentInfo.interpolationQueue->Wait(presentInfo.compositionFenceGPU, freeValue);

        auto commands = presentInfo.commandPool.get(presentInfo.interpolationQueue, L"HelixSR frame generation");
        if (commands != nullptr)
        {
            auto list = commands->reset();

            ffxDispatchDescFrameGeneration desc {};
            desc.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
            desc.commandList = list;
            desc.presentColor = ffxApiGetResourceDX12(realFrame, FFX_API_RESOURCE_STATE_PRESENT);
            for (UINT i = 0; i < count; i++)
                desc.outputs[i] = ffxApiGetResourceDX12(interpolationOutputs[outputSet][i].resource,
                                                        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            desc.numGeneratedFrames = count;
            desc.reset = frameInterpolationResetCondition;
            desc.backbufferTransferFunction = backBufferTransferFunction;
            desc.minMaxLuminance[0] = minLuminance;
            desc.minMaxLuminance[1] = maxLuminance;
            desc.generationRect = interpolationRect;
            desc.frameID = currentFrameID;

            if (frameGenerationCallback != nullptr &&
                frameGenerationCallback(&desc, frameGenerationCallbackContext) == FFX_API_RETURN_OK)
            {
                commands->execute(true);
                generated = (std::min)(desc.numGeneratedFrames, count);
            }
            else
            {
                commands->drop(true);
            }
        }
        else
        {
            LOG_ERROR("HelixSR swapchain: no free command list for frame generation");
        }
    }

    // after the wait on the game fence: the present queue's wait on this value also covers the game's frame
    presentInfo.interpolationQueue->Signal(presentInfo.interpolationFence, ++interpolationFenceValue);

    if (generated > 0)
        frameInterpolationResetCondition = false;

    return generated;
}

void HelixSRSwapChainDX12::presentInterpolated(UINT SyncInterval, UINT)
{
    const bool bVsync = SyncInterval > 0;

    // the generation waits for the game's frame
    presentInfo.gameQueue->Signal(presentInfo.gameFence, ++gameFenceValue);
    presentInfo.interpolationQueue->Wait(presentInfo.gameFence, gameFenceValue);

    // VSync or no tearing (fixed refresh): no more presents per second than the display shows
    UINT requested = generatedFrames;
    if ((bVsync || !tearingSupported) && refreshRate > 1.0 && realFrameSeconds > 0.0)
    {
        const double realFps = 1.0 / realFrameSeconds;
        UINT cap = (UINT) std::clamp((int) std::floor(refreshRate * 1.02 / realFps) - 1, 1, (int) kMaxGeneratedFrames);

        // a lower cap is taken after 30 real frames in a row, a higher one after 120 (no flipping at the boundary)
        if (cap != refreshCap)
        {
            refreshCapFrames++;
            if (refreshCapFrames >= (cap < refreshCap ? 30u : 120u) || refreshCap == 0)
            {
                LOG_INFO("HelixSR swapchain: {:.1f} fps at {:.0f} Hz: at most {} generated frame(s) per real frame",
                         realFps, refreshRate, cap);
                refreshCap = cap;
                refreshCapFrames = 0;
            }
        }
        else
        {
            refreshCapFrames = 0;
        }

        if (refreshCap > 0)
            requested = (std::min)(requested, refreshCap);
    }

    ID3D12Resource* realFrame = replacementSwapBuffers[replacementSwapBufferIndex].resource;
    const UINT generated = dispatchGeneration(requested, realFrame);

    EnterCriticalSection(&presentInfo.criticalSectionScheduledFrame);

    PacingData entry {};
    entry.vsync = bVsync;
    entry.tearingSupported = tearingSupported;
    entry.numFramesSentForPresentationBase = framesSentForPresentation;
    entry.interpolationCompletedFenceValue = interpolationFenceValue;
    entry.currentFrameID = currentFrameID;

    // generated frames: presentIndex base + 1 .. base + generated
    for (UINT i = 0; i < generated; i++)
    {
        PacingData::FrameInfo& fi = entry.frames[PacingData::FrameType::Generated_1 + i];
        fi.doPresent = true;
        fi.resource = interpolationOutputs[outputSet][i].resource;
        fi.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        fi.interpolationCompletedFenceValue = interpolationFenceValue;
        fi.presentIndex = ++framesSentForPresentation;

        // free again once the presenter copied it
        interpolationOutputs[outputSet][i].availabilityFenceValue = fi.presentIndex;
    }

    // real: presentIndex base + generated + 1
    if (!presentInterpolatedOnly && realFrame != nullptr)
    {
        PacingData::FrameInfo& fiReal = entry.frames[PacingData::FrameType::Real];
        fiReal.doPresent = true;
        fiReal.resource = realFrame;
        fiReal.state = D3D12_RESOURCE_STATE_PRESENT;
        fiReal.interpolationCompletedFenceValue = interpolationFenceValue;
        fiReal.presentIndex = ++framesSentForPresentation;
    }

    entry.replacementBufferFenceSignal = framesSentForPresentation;
    entry.numFramesToPresent = UINT32(framesSentForPresentation - entry.numFramesSentForPresentationBase);

    presentInfo.resetTimer = frameInterpolationResetCondition;
    presentInfo.scheduledInterpolations = entry;
    LeaveCriticalSection(&presentInfo.criticalSectionScheduledFrame);

    // kick off the interpolation thread (pacing) and the presenter
    SetEvent(presentInfo.presentEvent);

    nextPresentWaitValue = entry.numFramesSentForPresentationBase;

    if (replacementFrameLatencyWaitableObjectHandle)
    {
        UINT64 frameLatencyObjectWaitValue =
            (entry.numFramesSentForPresentationBase - 1) * (entry.numFramesSentForPresentationBase > 0);
        presentInfo.presentFence->SetEventOnCompletion(frameLatencyObjectWaitValue,
                                                       replacementFrameLatencyWaitableObjectHandle);
    }
}

HRESULT HelixSRSwapChainDX12::presentImpl(UINT SyncInterval, UINT Flags)
{
    if (Flags & DXGI_PRESENT_TEST)
        return presentInfo.swapChain->Present(SyncInterval, Flags);

    setConfig(nextConfig);

    EnterCriticalSection(&criticalSection);

    const UINT64 previousFramesSentForPresentation = framesSentForPresentation;
    const UINT currentBackBufferIndex = (UINT) replacementSwapBufferIndex;

    // the real frame time (for the refresh cap), measured between Presents
    {
        int64_t now = 0, freq = 1;
        QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&now));
        QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&freq));
        if (lastPresentQpc > 0)
        {
            const double dt = double(now - lastPresentQpc) / double(freq);
            realFrameSeconds = (dt > 0.1 || realFrameSeconds <= 0.0) ? dt : realFrameSeconds * 0.9 + dt * 0.1;
        }
        lastPresentQpc = now;
    }

    const bool runInterpolation = interpolationEnabled && frameGenerationCallback != nullptr;

    // the presenter has queued the copies of every earlier present (CPU fence) before the game queue waits on the
    // GPU fence: no GPU wait is queued for a value that is not queued to be signaled
    waitForFenceValue(presentInfo.compositionFenceCPU, previousFramesSentForPresentation);
    presentInfo.gameQueue->Wait(presentInfo.compositionFenceGPU, previousFramesSentForPresentation);

    if (runInterpolation)
    {
        // the interpolation thread took the previous real frame's entry and its generation finished
        WaitForSingleObject(presentInfo.interpolationEvent, INFINITE);

        presentInterpolated(SyncInterval, Flags);
    }
    else
    {
        presentPassthrough(SyncInterval, Flags);

        if (replacementFrameLatencyWaitableObjectHandle)
        {
            UINT64 frameLatencyObjectWaitValue = (framesSentForPresentation - gameMaximumFrameLatency) *
                                                 (framesSentForPresentation >= gameMaximumFrameLatency);
            presentInfo.presentFence->SetEventOnCompletion(frameLatencyObjectWaitValue,
                                                           replacementFrameLatencyWaitableObjectHandle);
        }
    }

    replacementSwapBuffers[currentBackBufferIndex].availabilityFenceValue = framesSentForPresentation;

    presentCount++;
    outputSet = (UINT) (presentCount % kOutputSets);
    replacementSwapBufferIndex = (int) (presentCount % (gameBufferCount > 0 ? gameBufferCount : 1));

    LeaveCriticalSection(&criticalSection);

    // the next back buffer is free once its last real frame was copied
    waitForFenceValue(presentInfo.replacementBufferFence,
                      replacementSwapBuffers[replacementSwapBufferIndex].availabilityFenceValue, INFINITE,
                      presentInfo.waitCallback);

    return S_OK;
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::Present(UINT SyncInterval, UINT Flags)
{
    return presentImpl(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::Present1(UINT SyncInterval, UINT PresentFlags,
                                                         const DXGI_PRESENT_PARAMETERS*)
{
    return presentImpl(SyncInterval, PresentFlags);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    EnterCriticalSection(&criticalSection);

    HRESULT hr = E_FAIL;

    if (riid == IID_ID3D12Resource || riid == IID_ID3D12Resource1 || riid == IID_ID3D12Resource2)
    {
        if (Buffer < gameBufferCount && verifyBackbufferDuplicateResources() &&
            replacementSwapBuffers[Buffer].resource != nullptr)
        {
            ID3D12Resource* pBuffer = replacementSwapBuffers[Buffer].resource;

            pBuffer->AddRef();
            *ppSurface = pBuffer;

            hr = S_OK;
        }
    }

    LeaveCriticalSection(&criticalSection);

    return hr;
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    auto hr = real()->SetFullscreenState(Fullscreen, pTarget);
    refreshRate = getRefreshRate(real());
    return hr;
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    return real()->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    HRESULT hr = real()->GetDesc(pDesc);

    // the game sees its own description
    if (SUCCEEDED(hr))
    {
        pDesc->BufferCount = gameBufferCount;
        pDesc->Flags = gameFlags;
        pDesc->SwapEffect = gameSwapEffect;
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                              DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    return resizeImpl(BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

// (ResizeBuffers and ResizeBuffers1 are hooked by OptiScaler: neither calls the other)
HRESULT HelixSRSwapChainDX12::resizeImpl(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                         UINT SwapChainFlags)
{
    destroyReplacementResources();

    EnterCriticalSection(&criticalSection);

    const UINT fiAdjustedFlags = getInterpolationEnabledSwapChainFlags(SwapChainFlags);

    if (BufferCount > 0)
        gameBufferCount = (std::min)(BufferCount, kMaxBufferCount);

    gameFlags = SwapChainFlags;

    HRESULT hr = real()->ResizeBuffers(0 /* preserve count */, Width, Height, NewFormat, fiAdjustedFlags);
    refreshRate = getRefreshRate(real());

    LeaveCriticalSection(&criticalSection);

    return hr;
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return real()->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetContainingOutput(IDXGIOutput** ppOutput)
{
    HRESULT hr = DXGI_ERROR_INVALID_CALL;

    if (ppOutput)
    {
        *ppOutput = getMostRelevantOutputFromSwapChain(real());
        hr = *ppOutput != nullptr ? S_OK : DXGI_ERROR_NOT_FOUND;
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return real()->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetLastPresentCount(UINT* pLastPresentCount)
{
    return real()->GetLastPresentCount(pLastPresentCount);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    HRESULT hr = real()->GetDesc1(pDesc);

    if (SUCCEEDED(hr))
    {
        pDesc->BufferCount = gameBufferCount;
        pDesc->Flags = gameFlags;
        pDesc->SwapEffect = gameSwapEffect;
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    return real()->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetHwnd(HWND* pHwnd) { return real()->GetHwnd(pHwnd); }

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return real()->GetCoreWindow(refiid, ppUnk);
}

BOOL STDMETHODCALLTYPE HelixSRSwapChainDX12::IsTemporaryMonoSupported(void) { return real()->IsTemporaryMonoSupported(); }

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return real()->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return real()->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return real()->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return real()->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return real()->GetRotation(pRotation);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::SetSourceSize(UINT Width, UINT Height)
{
    return real()->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return real()->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::SetMaximumFrameLatency(UINT MaxLatency)
{
    gameMaximumFrameLatency = MaxLatency;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    if (pMaxLatency)
        *pMaxLatency = gameMaximumFrameLatency;

    return pMaxLatency ? S_OK : DXGI_ERROR_INVALID_CALL;
}

HANDLE STDMETHODCALLTYPE HelixSRSwapChainDX12::GetFrameLatencyWaitableObject(void)
{
    return replacementFrameLatencyWaitableObjectHandle;
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return real()->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return real()->GetMatrixTransform(pMatrix);
}

UINT STDMETHODCALLTYPE HelixSRSwapChainDX12::GetCurrentBackBufferIndex(void)
{
    EnterCriticalSection(&criticalSection);
    UINT result = (UINT) replacementSwapBufferIndex;
    LeaveCriticalSection(&criticalSection);

    return result;
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                       UINT* pColorSpaceSupport)
{
    return real()->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    switch (ColorSpace)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        backBufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        backBufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        backBufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
        break;
    default:
        break;
    }

    return real()->SetColorSpace1(ColorSpace);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                               DXGI_FORMAT Format, UINT SwapChainFlags,
                                                               const UINT*, IUnknown* const*)
{
    // one present queue of our own: the per-buffer queues and node masks the game asks for do not apply
    return resizeImpl(BufferCount, Width, Height, Format, SwapChainFlags);
}

HRESULT STDMETHODCALLTYPE HelixSRSwapChainDX12::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
    if (Size > 0 && pMetaData != nullptr && Type == DXGI_HDR_METADATA_TYPE_HDR10)
    {
        auto* hdr10 = static_cast<DXGI_HDR_METADATA_HDR10*>(pMetaData);
        minLuminance = hdr10->MinMasteringLuminance / 10000.0f;
        maxLuminance = float(hdr10->MaxMasteringLuminance);
    }

    return real()->SetHDRMetaData(Type, Size, pMetaData);
}
