# Private NR cache preparation

`prepare.py` reads the supported local `nvngx_dlssnr.dll`, extracts its model
data without loading the DLL, records the pinned MIT OpenDLSS-NR graph on the
CPU, and translates its compute shaders to D3D12 shader model 6.2. Preparation
needs no GPU, Wine, WebGPU implementation, npm packages, or NVIDIA driver.

The supported model is the tested **310.8.SF.0** DLL with SHA256
`6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927`.
Other DLLs are rejected before extraction. Obtain the DLL from your own local
installation; this repository includes neither the DLL nor NVIDIA model data.

Requirements:

- Python 3.10 or later, using only its standard library.
- Node.js 24 (tested with 24.22.1).
- `naga-cli` 30.0.1, for example `cargo install naga-cli --version 30.0.1 --locked`.
- Microsoft DXC 1.9.2609.5 (the 1.9.2609 release).

Both Linux and Windows tools are supported. Pass executable paths explicitly
when they are outside `PATH`. The parent of the new output directory must exist.
From this directory, for example:

```text
python prepare.py --dll "/path/to/nvngx_dlssnr.dll" --output "/private/model-cache" --width 320 --height 320 --node "/path/to/node" --naga "/path/to/naga" --dxc "/path/to/dxc"
```

The default `--math accurate` preserves the reference's reduction arithmetic.
BC250-tuned thread ownership and redundant shared-memory-clear removal reduce
the measured full-network GPU time from 109.6 ms to 98.5 ms, with identical
output on the tested inputs. Source hashes restrict these transforms to the
audited shaders in the pinned graph.

An optional `--math fast` cache uses IEEE sums inside the same half-publication
groups. It measures 62.9 ms on BC250, but changes the model's numerical result.
Synthetic final RGB RMSE against accurate mode is 0.0022–0.0048 across the
tested temporal/reset/resize scenes; this does not establish quality for other
content. Prepare a separate output directory for each mode and select its
`ModelCache` path. Modes are recorded in `preparation.json` and use distinct
compiled-shader keys. Accurate remains the default.

For a larger performance tradeoff, `--field compact` lowers the padded-field
floor from 320 to 128. For example, prepare `--width 192 --height 108
--math fast --field compact` into a separate cache, then set NR resolution
to 192 in Scaler. Its field is 192x128. The isolated BC250 game test reaches
about 29 FPS, compared with about 14.2 FPS for fast/native 320x180 and 9.1 FPS
for accurate/native. These are observations in one saved scene.

Compact geometry changes which tokens and context the model sees, in addition
to lowering input resolution. It is not equivalent to the original model.
Synthetic RGB RMSE is 0.018–0.023 versus accurate/native in the tested scenes,
with maximum difference 0.112. Check image quality for your content. The
default `--field native` keeps the original field rule. Selecting a smaller
NR resolution alone cannot shrink an already prepared cache's field.

The width and height describe the **network input**, independently of the game
display resolution. Scaler's NR resolution setting selects the longest network
edge and preserves the display aspect ratio, with a minimum of 33 on each axis.
For example, a 16:9 display and the default NR resolution of 320 use 320x180.
With default native geometry, the padded field is at least 320x320, so reducing
valid input below that field does not reduce the neural compute workload.

The generated cache contains `runtime-plan.bin`, `initial-data.bin`, `cso/`, and
`preparation.json`. Set `[NeuralRendering] ModelCache` to its directory. The
default is `HelixSR-NR/model-cache` beside the Scaler DLL. Install the separately
built `helixsr-nr.dll` in `HelixSR-NR/`. Existing output directories are never
overwritten. On a failed preparation, temporary model data is removed and no
partial cache is published.

The manifest records the source, tool versions, arithmetic mode and field mode. The runtime verifies the
static data and every compiled shader against SHA256 digests in its bounded
execution plan. These digests detect cache corruption; they do not authenticate
an untrusted cache supplier.

Generated caches contain NVIDIA model data and must remain private. Do not
commit, upload, or include them in releases. Ignore rules are a safeguard, not
permission to redistribute that data. See the retained third-party licenses
and source revisions in [vendor/SOURCES.txt](vendor/SOURCES.txt).

CPU preparation is not a GPU correctness test. Build the backend with
`HELIXSR_NR_BUILD_PROBE=ON` and run `frame_probe.exe <absolute-cache-directory>`
on BC250 to validate actual GPU execution, resource retirement, command-list
replay, cross-queue ordering, resize, and SDR/HDR handling. The probe generates
its own synthetic scene; game validation remains a separate requirement.
For smaller caches, pass the valid input dimensions explicitly, for example
`frame_probe.exe <compact-cache-directory> 192 108`. The default is 320x320.
