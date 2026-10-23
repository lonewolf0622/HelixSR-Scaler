# Neural Rendering backend

This directory contains the optional D3D12 NR backend and private model
preparation tools. Scaler runs the pass after upscaling and output scaling,
before sharpening and the magnifier. It works with the released HelixSR FFX
provider as well as Scaler's other D3D12 upscalers. It is disabled by default.

The backend executes the 71-block model as ordinary compute shaders. It needs
shader model 6.2 and native 16-bit operations. It does not load the NVIDIA NR
DLL, call CUDA, or require Tensor Cores. Its graph/shader source is derived from
the pinned MIT OpenDLSS-NR port; third-party notices are retained alongside it.

## Build and local setup

Build Scaler normally from the full repository with its pinned submodules.
Build the separate backend with a Windows C++17 compiler and DXC:

```text
cmake -S helixsr/nr/runtime -B helixsr/nr/runtime/build -A x64 -DDXC_EXECUTABLE=/path/to/dxc.exe
cmake --build helixsr/nr/runtime/build --config Release
```

Place the resulting `helixsr-nr.dll` in `HelixSR-NR/` beside the Scaler DLL.
Prepare a private model cache following [tools/README.md](tools/README.md).
Set `[NeuralRendering] ModelCache` to that directory, or use the default
`HelixSR-NR/model-cache`, then enable Neural Rendering in Scaler's menu or
`OptiScaler.ini`. Source, toolchain, and generated cache are independent:
neither NVIDIA binaries nor generated model data belong in this repository
or a release archive.

`NetworkSize` sets the longest network edge, preserving the display aspect
ratio. Default native caches have a minimum padded field of 320x320. Lowering the valid input
below that field does not reduce the network's GPU workload. A prepared graph
must cover the selected field. Intensity and conditioning values are read for
each frame. Zero intensity bypasses neural work and resets history before NR
resumes, while retaining an already prepared context. Loading runs on one
background worker; normal upscaling continues
while loading or when the optional backend is unavailable.

## Resource and submission contract

The C ABI is in [runtime/HelixSrNrApi.h](runtime/HelixSrNrApi.h), version 2.
The caller records with `Record`, calls `PrepareSubmission` immediately before
the actual `ExecuteCommandLists`, then calls `Submitted` immediately after it.
This sequence must be serialized per context and repeated for command-list
replay. Different actual queues are ordered with a GPU fence wait.

`RetireRecording` closes a recording only when reset/release prevents replay.
Its GPU resources remain retained until completion. `Abort` discards work that
was never submitted. `Destroy` returns `E_PENDING` while a recording remains
open, otherwise waits for submitted work. A failed destruction preserves the
context. The Scaler adapter supplies queue hooks, lifetime watches, reset
notifications, and deferred retirement for this contract.

Game resource states are managed across the entire SR/post-processing pipeline
and restored on return, including evaluation failures. Legacy compatibility
barriers are skipped while this shared management is active. Internal texture
copies retain their own transitions. Reused scaling, sharpening, and magnifier
textures retain their last actual state until an explicit transition changes it.

## Current validation and limits

See [VALIDATION.md](VALIDATION.md) for the measured checks and their scope.

Windows MSVC builds have run on BC250 through Wine/vkd3d-proton. Tests cover
the complete Scaler NGX path with both FSR 2.2 and the released HelixSR 2.0
Model E network before NR. They verify real automatic queue submission and
retirement, replay, cross-queue ordering, resize, reset, missing motion,
SDR/HDR intensity handling, explicit compatibility states, Unreal's inferred
states, and the combined scaling/NR/sharpening/magnifier path.

Color/output support includes RGBA16F, RGBA32F, R11G11B10F, RGBA8 UNORM, and
RGB10A2 UNORM, with the corresponding typeless resource families. Motion can
use two- or four-channel FP16/FP32 textures, including typeless float families.
Depth supports R32F, R16F/UNORM and shader-readable typeless depth families.
Every typed view is checked against the actual device's load/store support.
Textures currently need one mip, one layer and no MSAA. Integer guide formats
are rejected before recording. Depth and motion subrect origins are respected.
Neutral style, proxy, linear SDR, and scene-linear HDR are supported.
In default accurate mode, the complete neural output matches the independent Vulkan reference exactly
when supplied identical features. This does not establish exact NVIDIA image
parity: cross-backend noise preparation and final images differ.

The minimum 320x320 field currently takes about 98.5 ms for the network on
the tested BC250. The isolated CONTROL Resonant test has also reached a saved
scene with released HelixSR SR followed by NR, using R11G11B10F output, R32
typeless depth and RG16F motion. The earlier 109.6 ms cache ran at roughly 9 fps.
This establishes a functional game path, not production image-quality or
general game compatibility. Broader validation and performance work remain
in progress. First-time shader preparation can take several minutes on BC250;
SR continues while the background worker prepares NR.

Private cache preparation also offers optional fast arithmetic, measured at
62.9 ms. It changes numerical results; see [tools/README.md](tools/README.md)
and [VALIDATION.md](VALIDATION.md) for the image checks and their limits.
Optional compact geometry lets a smaller prepared field reduce actual work,
at the cost of changing model context. The tested 192x108 compact/fast game
configuration reaches about 29 FPS. Native geometry and accurate arithmetic
remain the defaults.

## Validation callers

Configure with `-DHELIXSR_NR_BUILD_PROBE=ON` to build `frame_probe.exe`,
`format_probe.exe`, and `scaler_probe.exe` and enable diagnostic exports.
Diagnostics default off.
The full repository is needed for the optional Scaler caller's existing NGX
headers and Detours library. The probes generate their own synthetic input.

- `frame_probe.exe <absolute-cache-directory> [network-width network-height]`
  exercises the backend directly; dimensions default to 320x320.
- `format_probe.exe <absolute-cache-directory>` checks nine typed/typeless
  color formats, eight motion/depth combinations, zero-intensity preservation
  including wide HDR values, and guide subrects with deliberately different
  padding. Seven depth families are compared separately, including R16,
  R24G8 and R32G8X24 typeless depth. It also rejects unsupported integer depth
  before recording.
- `scaler_probe.exe` loads the complete Scaler as `nvngx.dll`, expecting NR
  enabled and its backend installed normally.
- `scaler_probe.exe --audit-states` independently observes actual texture
  transitions. Configure `[Hotfix] ColorResourceBarrier`,
  `MotionVectorResourceBarrier`, and `DepthResourceBarrier` to `2048`, and
  `OutputResourceBarrier` to `1024`, matching the caller's input/output states.
- `scaler_probe.exe --unreal-states` initializes the public Unreal NGX path
  and tests its inferred states without explicit Hotfix overrides.

Both state audit modes also deliberately fail an evaluation and verify the
original output parameter and caller states are restored. Scaling, CAS, and
a static magnifier can be enabled in the same test configuration to exercise
the complete post-processing chain.
