The dedicated FG package places `helixsr_fg.dll` next to
`HelixSR-fsrfg/amd_fidelityfx_dx12.dll`, `helixsr.ini`, and the `helixsr-fg`
dependency folder. Scaler selects the dedicated DLL before any legacy combined
provider. Existing 2.0 combined providers work when the dedicated DLL is absent.
A present DLL that fails loading, has an incompatible `HelixSR_NgxFgVersion`,
or lacks a required DX12 NGX entry point stops selection and logs a package
repair error. Scaler exports `HelixSR_SeparateFgLoaderVersion` (1) for installer
pairing checks.

Use `FGInput=nvngxfg`, `FGNvngxReplacement=HelixSR`: the game's Streamline keeps
presentation ownership. No additional swapchain is introduced. HelixSR receives
the original NGX depth resource, bypassing the COPY_DEST workaround intended for
Nukem's provider. This avoids imposing a different resource-state contract and
an unfenced two-resource copy pool on HelixSR.

The DLSSG output helper waits up to five seconds for an allocator slot's previous
submission before resetting it. UI fence values are assigned when submitted;
SC submissions now signal a monotonic fence too. Failed signals poison a slot
instead of permitting unsafe reuse. This applies to Scaler's DLSSG output helpers;
frame generation's executor and Streamline own their separate resource lifetimes.

CPU checks execute production loader selection and allocator methods against
mock providers/queues, including invalid dedicated providers, fallback, timeout,
device loss, and signal failures:

    python3 helixsr/tests/test_fg_loader.py
    python3 helixsr/tests/test_fg_allocator.py

These checks and a cross-build establish interface/control-flow correctness.
They do not establish GPU correctness, pacing, visual quality, or gameplay stability.
