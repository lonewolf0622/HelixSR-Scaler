// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>
#include <stdint.h>

#ifdef HELIXSR_NR_BUILD
#define HELIXSR_NR_API __declspec(dllexport)
#else
#define HELIXSR_NR_API __declspec(dllimport)
#endif

typedef struct HelixSrNrContext HelixSrNrContext;
typedef void (*HelixSrNrProgress)(void* user, uint32_t completed, uint32_t total);

typedef struct HelixSrNrCreateInfo {
    uint32_t size;
    uint32_t apiVersion; // 2
    ID3D12Device* device;
    const wchar_t* cacheDirectory;
    const wchar_t* planFile;
    const wchar_t* dataFile;
    uint32_t displayWidth, displayHeight;
    uint32_t networkWidth, networkHeight;
    uint32_t maximumFramesInFlight; // 1..16
    HelixSrNrProgress progress;
    void* progressUser;
} HelixSrNrCreateInfo;

typedef struct HelixSrNrRegion {uint32_t x, y, width, height;} HelixSrNrRegion;
typedef struct HelixSrNrFrameInfo {
    uint32_t size;
    ID3D12GraphicsCommandList* commands;
    // Color and output use the encoding selected by colorSpace. The backend
    // constructs the display proxy and preserves scene-linear HDR highlights.
    ID3D12Resource* color;
    ID3D12Resource* depth; // optional; absence disables disocclusion rejection
    ID3D12Resource* motion; // optional; absence disables history for this frame
    ID3D12Resource* output;
    D3D12_RESOURCE_STATES colorState, depthState, motionState, outputState;
    HelixSrNrRegion colorRegion, depthRegion, motionRegion;
    float motionScaleX, motionScaleY; // encoded motion -> display UV displacement
    uint32_t seed, reset, depthInverted, autoMask;
    float intensity, style, localTone, localStructure, skinStructure;
    float historyBlend, depthThreshold;
    uint32_t colorSpace; // 0 display-proxy code, 1 linear HDR, 2 linear SDR
    float paperWhite; // positive scene-relative white for HDR proxy construction
} HelixSrNrFrameInfo;

#ifdef __cplusplus
extern "C" {
#endif
// Creation uploads and retires immutable data on a private queue. It can be
// called on a worker while the optional NR pass remains disabled in the host.
HELIXSR_NR_API HRESULT HelixSR_NR_Create(const HelixSrNrCreateInfo*, HelixSrNrContext**);
// Record modifies compute roots/heaps. The caller restores its own GPU state.
// Call PrepareSubmission immediately before ExecuteCommandLists, and Submitted
// immediately after, on every execution including command-list replay. Serialize
// this sequence per context. Different queues are ordered by a GPU fence wait.
// RetireRecording only after Reset/release makes further replay impossible.
// Abort is for discarded work that was never submitted.
HELIXSR_NR_API HRESULT HelixSR_NR_Record(HelixSrNrContext*, const HelixSrNrFrameInfo*, uint64_t* ticket);
HELIXSR_NR_API HRESULT HelixSR_NR_PrepareSubmission(HelixSrNrContext*, uint64_t ticket, ID3D12CommandQueue*);
HELIXSR_NR_API HRESULT HelixSR_NR_Submitted(HelixSrNrContext*, uint64_t ticket, ID3D12CommandQueue*);
HELIXSR_NR_API HRESULT HelixSR_NR_RetireRecording(HelixSrNrContext*, uint64_t ticket);
HELIXSR_NR_API HRESULT HelixSR_NR_Abort(HelixSrNrContext*, uint64_t ticket);
HELIXSR_NR_API HRESULT HelixSR_NR_Collect(HelixSrNrContext*);
// Returns E_PENDING if there is an unsubmitted or still replayable record. Otherwise waits for
// submitted GPU work before releasing resources. Failure preserves the handle.
HELIXSR_NR_API HRESULT HelixSR_NR_Destroy(HelixSrNrContext*);
// UTF-8 error text for the last failing call on this CPU thread.
HELIXSR_NR_API const char* HelixSR_NR_LastError(void);
#ifdef __cplusplus
}
#endif
