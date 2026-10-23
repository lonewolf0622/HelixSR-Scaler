# BC250 validation

Validated on 2026-10-23 with Windows MSVC builds running through Wine /
vkd3d-proton on an AMD BC250. These results describe this hardware and the
tested scenes; they do not establish native NVIDIA image parity or broad
game compatibility.

## Functional checks

- Full Scaler NGX path with FSR 2.2 and the released, unmodified HelixSR 2.0
  Model E provider followed by the NR model.
- Actual automatic queue submission, retirement after reset/release,
  command-list replay, cross-queue ordering, discarded recordings, reset,
  resize, missing motion, and invalid input rejection.
- An independent observer checks real D3D12 texture barriers, rather than
  relying on the implementation's tracked state. The combined released
  HelixSR -> output scaling -> NR -> RCAS -> magnifier test observes 2,392
  transitions with zero errors. An intentionally failed evaluation restores
  the original output parameter and caller resource states.
- Nine color/output formats preserve the synthetic input exactly at zero
  intensity, including FP32 HDR values above 65,504. R11G11B10F output is
  finite and differs from the input with the network enabled.
- Eight motion format variants and seven depth families produce the same
  temporal result. Poisoned padding around nonzero guide subrects is ignored.
  Unsupported integer depth is rejected before recording.
- Linear SDR/HDR zero-intensity preservation and nonzero HDR highlights pass.
- The complete neural head matches the independent Vulkan reference exactly
  with identical features. The feature-preparation noise differs in 106 half
  values; final RGB differs from the native NVIDIA synthetic reference
  (RMSE 0.002322, maximum absolute difference 0.028137). No exact native
  NVIDIA image-parity claim is made.

## Performance

The 451-dispatch, minimum 320x320 field was measured with the game closed,
alternating two runs per variant and averaging three warm GPU frames per run.
Scaled, ties-to-even half-subnormal rounding reduces network time from
112.624 ms to 109.592 ms (2.69%) while preserving every output word in all
four repeated full-model outputs. The conversion also passed 184,549,376
exhaustive signed subnormal comparisons and 23,068,672 hashed comparisons
across all float exponents, with no bit differences.

A subsequent layout pass uses 256 threads for the matrix families and removes
audited redundant workgroup initialization in GEMM and window attention.
Across two alternating runs per variant, network time falls from 109.595 ms
to 98.462 ms (10.16% less GPU time, 1.113x throughput). All repeated complete
heads remain exact. Fresh caches built by the production preparation tools
also reproduce every tested final pixel through temporal frames, reset,
resize, command-list replay and cross-queue submission. Compiler resource
captures show no register spills or scratch in either variant; the largest
VGPR allocation falls from 128 to 100. No spill-removal speedup is claimed.

Optional fast arithmetic, with the same layout changes, measures 62.908 ms
(1.742x the 109.595 ms baseline). It omits Ada F13 product truncation and is
not bit-equivalent to the reference. Its synthetic final RGB RMSE against
accurate mode is 0.00222–0.00474, maximum absolute difference 0.03189, across
three temporal frames, reset and a resized scene. These are limited image
checks, not a general perceptual-quality guarantee. Accurate is the default.
Explicit fused arithmetic and forced wave32 measured slower and were rejected.

The isolated CONTROL Resonant saved-scene test runs released HelixSR SR at
640x360 -> 960x540, followed by NR at 320x180 in the padded 320x320 field.
The production backend with the optimized private cache reaches gameplay.
NR is expensive: the configuration runs at roughly 9 FPS. This is functional
compatibility, not a real-time performance claim.

Fresh optimized accurate and fast caches also reach the same saved scene and
pass a camera-motion check. At the matching post-motion view, the live Scaler
overlay shows about 109.3 ms / 9.1 FPS for accurate and 70.3 ms / 14.2 FPS for
fast. These are scene-specific observations rather than a repeated game
benchmark. No obvious visual failure was observed in those captures; they
do not establish quality across other scenes or extended motion.

An explicit compact-field experiment at 192x108 input in a 192x128 field
reaches the same saved scene and passes the camera-motion/lifecycle checks.
Its live overlay shows approximately 33.8 ms / 29 FPS with HelixSR active.
This option changes both input resolution and the model's token context.
Synthetic final RGB RMSE against accurate/native is 0.0178–0.02295, maximum
absolute difference 0.11176. It is a performance/quality tradeoff, not native
equivalence. Native geometry remains the default.

On CPU-prepared initial features with the game closed, paired full-network
runs measure 60.30 ms for fast 320x320, 31.33 ms for compact/fast 192x192,
and 27.12 ms for compact/fast 192x108 (192x128 field). These inputs differ
from the captured-feature fixture used in the 62.908 ms measurement, so
the timing series are reported separately. Game observations above are
also separate from these isolated GPU measurements.

The real menu was tested for live disable/re-enable, intensity changes,
320 -> 288 -> 320 resolution changes, and resuming from zero intensity. The
zero-intensity bypass restores the 30 FPS test cap. A warm restart with the
original validated cache activated NR about 18 seconds after renderer
initialization, versus about 159 seconds cold; these are observations, not
startup guarantees. SR remains available during background preparation.

## Build and packaging

Full Scaler MSVC and standalone production/probe CMake builds pass. Existing
Windows/Linux installer tests, upscaler names/frame-generation settings, and
91 CPU resource-state checks pass. The release workflow builds the optional
backend and packages only tracked preparation sources and retained licenses.
The package contains no NVIDIA DLL, model weights, compiled model shaders,
or private cache. GPU probes require a user-prepared cache and BC250; they
are not run by ordinary GitHub-hosted CI.
