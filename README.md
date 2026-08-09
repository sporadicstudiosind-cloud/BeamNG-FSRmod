# BeamNG FSR Bridge

Two independent products for BeamNG.drive's Direct3D 12 renderer:

| Product | What it adds | Install |
| --- | --- | --- |
| **AMD FSR Frame Generation** | ML frame generation on RDNA 4, analytical frame generation elsewhere, plus AMD Radeon Anti-Lag 2 | `dist/framegen/` |
| **AMD FSR Upscaling** | ML upscaling on RX 9000 / RX 7000, analytical FSR 3.1.x elsewhere | `dist/upscaling/` |

**Each works completely on its own.** Neither requires the other, neither knows
whether the other is installed, and installing both is supported — they ship the
same bridge binaries, so copying one over the other is a no-op. What differs is
a one-line module manifest, and the shared runtime starts a feature only when
its manifest is present.

If you only want one thing, install only that one thing.

---

## Read this first

**A pure BeamNG mod cannot do this.** BeamNG's Lua sandbox denies `ffi.load` and
`package.loadlib` — confirmed by a BeamNG developer on the official forums — so
no mod ZIP can call the FSR DLLs. BeamNG's PostFX system is pixel-shader only
and cannot run compute, change render resolution, or apply projection jitter.
Any project claiming to implement FSR 3/4 as a normal mod is mistaken.

So this project is **two halves**: a normal BeamNG mod, and a native Direct3D 12
bridge that runs alongside it. The full reasoning, with sources, is in
[`docs/FEASIBILITY.md`](docs/FEASIBILITY.md).

**Nothing here has been tested on hardware.** No Windows machine, no BeamNG
install and no GPU were available while building it. What *was* verified is in
the audit table below, stated precisely. Everything else is marked NOT TESTED.

## What works, honestly

| | Assessment |
| --- | --- |
| **FSR Frame Generation** | The strongest feature. Frame generation does not need camera jitter, and everything it does need can be obtained. This is what to use. |
| **FSR Upscaling** | Runs, but **without camera jitter**, because BeamNG exposes no hook for it. Materially softer than a native FSR integration. Structural, unfixable from the mod side, and stated in the UI every time you enable it. |
| **Vulkan / D3D11** | Impossible. AMD ships no Vulkan FSR backend and FSR 3.1+ requires D3D12. The mod detects the renderer and disables itself with an explanation. |
| **AMD's driver-side FSR 4 upgrade** | BeamNG cannot qualify. AMD explicitly excludes third-party integrations. |

## The current FSR landscape, as shipped

Verified by reading AMD FSR SDK 2.3.0 "Redstone" directly:

| Technique | Version | Kind | GPUs | API |
| --- | --- | --- | --- | --- |
| FSR Upscaling (ML) | **4.1.1** | machine learning | RX 9000 **and RX 7000** | DX12, SM 6.6 |
| FSR Super Resolution | **3.1.5** | analytical | RX 500+ or equivalent | DX12, SM 6.2 |
| FSR Frame Generation (ML) | **4.0.1** | machine learning | RX 9000+, **Windows 11 only** | DX12, CS_6_6 |
| FSR Frame Generation | **3.1.6** | analytical | any SM 6.2 GPU | DX12 |
| FG Swapchain | **3.1.7** | DXGI replacement | — | DX12 |

Two things that are widely got wrong and that this project models correctly:
ML upscaling is **no longer RDNA 4 exclusive** (4.1.1 added RX 7000), and ML
frame generation is **still stricter** than ML upscaling (RX 9000 + Windows 11).
They are separate capability bits, and a unit test fails if they are ever
conflated.

## Install

Pick a product; each needs both of its halves. See [`docs/INSTALL.md`](docs/INSTALL.md).

```
dist/framegen/beamng_fsr_framegen.zip   →  %LOCALAPPDATA%\BeamNG.drive\<version>\mods\
dist/framegen/bridge/*                  →  ...\BeamNG.drive\Bin64\
```

Same shape for `dist/upscaling/`. No BeamNG file is modified, overwritten or
replaced. Uninstalling is deletion and nothing else
([`docs/UNINSTALL.md`](docs/UNINSTALL.md)) — and deleting just one product's
`beamng_fsr/modules/*.module.json` disables that product while leaving the other
working.

## How it works

```
dxgi.dll   (proxy) → wraps IDXGIFactory7 → sees swapchain creation
d3d12.dll  (proxy) → wraps ID3D12Device  → sees texture creation
                   ↓
         beamng_fsr_bridge.dll   one runtime, two independent modules
           ├─ framegen  ← started only if framegen.module.json exists
           └─ upscaling ← started only if upscaling.module.json exists
                   ↑ one file IPC channel per product
         BeamNG mod(s) → shared PostFX beacon publishes depth + motion vectors
```

The **module manifest** is what makes the products separable. Each ships one;
the runtime starts a feature only when its manifest is present. Install frame
generation alone and the swapchain is never even wrapped for upscaling, no
upscaling settings file is read, and no upscaling status is published.

The **beacon** is the key idea. Rather than the bridge guessing which of
hundreds of textures is the depth buffer, the mod half copies BeamNG's
`#prepass[Depth]` and `#VelocityBuffer[RT0]` into a target *it* created and
tells the bridge that target's exact description. The bridge then looks for
something it has a description of. That is what makes it survive BeamNG updates.

**Hard rule:** the native half hooks only Microsoft APIs. No BeamNG symbol,
offset, signature or binary is referenced, scanned, patched or depended upon.
This is both the legal position ([`docs/LEGAL.md`](docs/LEGAL.md)) and why
BeamNG updates should not break it.

Full detail: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Build

```bash
python3 tools/fetch_fsr_sdk.py --dest third_party/FidelityFX-SDK
cmake -S native -B native/build -DBFSR_BUILD_BRIDGE=OFF
cmake --build native/build && ./native/build/bfsr_tests    # 146 tests
```

Windows bridge and packaging: [`docs/BUILD.md`](docs/BUILD.md).

## Documentation

| | |
| --- | --- |
| [`FEASIBILITY.md`](docs/FEASIBILITY.md) | The research. Every architectural decision traces here. |
| [`ARCHITECTURE.md`](docs/ARCHITECTURE.md) | How the two halves fit together |
| [`LIMITATIONS.md`](docs/LIMITATIONS.md) | **Read before installing** |
| [`COMPATIBILITY_MATRIX.md`](docs/COMPATIBILITY_MATRIX.md) | Predicted support per GPU, all NOT TESTED |
| [`INSTALL.md`](docs/INSTALL.md) · [`UNINSTALL.md`](docs/UNINSTALL.md) | Setup and removal |
| [`TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) | Symptom → cause |
| [`TESTING.md`](docs/TESTING.md) | What is automated, and the hardware plan that is not |
| [`BUILD.md`](docs/BUILD.md) | Toolchain and layout |
| [`LEGAL.md`](docs/LEGAL.md) | EULA analysis and the AMD licence conflict |
| [`FUTURE_WORK.md`](docs/FUTURE_WORK.md) | What is next and why it was not first |

---

## Final audit

Nothing is marked verified that was not actually verified. "Verified" here means
a command was run and its output checked, not that the code looks right.

| Feature | Implemented | Verified | Hardware / API restrictions | Notes |
| --- | --- | --- | --- | --- |
| Feasibility analysis | ✅ | ✅ sourced to AMD, BeamNG and Microsoft documentation | — | BeamNG sandbox restriction confirmed by a BeamNG developer |
| FSR API usage is real and current | ✅ | ✅ compiles against AMD FSR SDK 2.3.0's actual headers | — | No API invented; SDK cloned and read |
| DXGI proxy (`dxgi.dll`) | ✅ | ✅ builds; export table matches system DLL | Windows x64 | Full `IDXGIFactory..7` chain wrapped |
| D3D12 proxy (`d3d12.dll`) | ✅ | ✅ builds; export table matches system DLL | Windows x64 | Base `ID3D12Device` only — see LIMITATIONS §4 |
| FSR upscaling integration | ✅ | ⚠️ compile-verified only | DX12; SM 6.2 / 6.6 | **Runs without camera jitter** — LIMITATIONS §1 |
| FSR frame generation | ✅ | ⚠️ compile-verified only | DX12; ML needs RX 9000 + Win 11 | Wraps AMD's interpolation swapchain |
| Runtime version discovery + selection | ✅ | ⚠️ compile-verified only | — | AMD's `ffxQuery` / `ffxOverrideVersion`; nothing hardcoded |
| GPU / API / OS / SM detection | ✅ | ✅ policy layer unit-tested | — | Adapter matched by LUID, not index |
| Capability model | ✅ | ✅ 15 unit tests, one per GPU class | — | Encodes AMD's published requirements |
| Two separately installable products | ✅ | ✅ 11 module tests + 5 independence tests + a CI co-install check | — | Manifest-gated; overlapping files verified byte-identical |
| Frame-generation multiplier (MFG) | ⚠️ reported, not settable | ✅ read from AMD's SDK | — | SDK 2.3.0 gives the app no way to request one; `numGeneratedFrames` arrives already decided. The panel shows the observed value. |
| Auto mode | ✅ | ✅ 27 unit tests | — | Uses AMD's own documented base-fps thresholds (60 fps for FG 3.1.6, 30 fps for FG 4.0.1) |
| Anti-Lag 2 | ✅ | ⚠️ MSVC builds only | AMD GPUs only | GCC/Clang can't compile AMD's header — guarded, reported unavailable |
| Settings persistence | ✅ | ✅ unit-tested incl. corrupt input | — | Atomic write; survives reload and renderer restart |
| Settings + diagnostics UI | ✅ | ✅ JS/JSON/HTML parse-checked | — | Not rendered in a real BeamNG UI |
| BeamNG Lua half | ✅ | ✅ LuaJIT syntax-checked | — | Not executed in a real BeamNG VM |
| PostFX beacon | ✅ | ⚠️ authored against documented API | Needs Lighting Quality ≥ Low | Target names from BeamNG's docs; not run |
| Failsafe / safe mode | ✅ | ✅ logic unit-tested | — | Crash sentinel; every entry point catches |
| Frame-gen gating and pacing | ✅ | ✅ 13 unit tests | — | Pause, load, menu, replay, photo, occlusion, hitch, cuts |
| Diagnostics + benchmark export | ✅ | ✅ 11 unit tests | — | Rendered vs displayed FPS never merged |
| Packaging | ✅ | ✅ executed end to end | — | Both artefacts produced; DLL hashes printed |
| Licensing / notices | ✅ | ✅ AMD licence read and reproduced | — | Binary-only redistribution; no AMD source vendored |
| Vulkan support | ❌ **not possible** | ✅ three independent confirmations | — | AMD ships no Vulkan backend |
| D3D11 support | ❌ **not possible** | ✅ | — | FSR 3.1+ requires D3D12 |
| Driver-side FSR 4 upgrade | ❌ **not possible** | ✅ per AMD's support article | — | AMD excludes third-party integrations |
| **Performance measurements** | ❌ | ❌ **no hardware** | — | Test plan provided; no numbers fabricated |
| **Image-quality comparison** | ❌ | ❌ **no hardware** | — | Method in TESTING.md §3.6 |
| **Per-GPU compatibility** | ❌ | ❌ **no hardware** | — | Every row NOT TESTED |
| Depth convention | ⚠️ assumed | ❌ **unverified** | — | Reversed-Z assumed, UI override, debug view to confirm |
| Motion-vector encoding | ⚠️ assumed | ❌ **unverified** | — | UV space + Y flip assumed, UI override, debug view to confirm |

### The honest summary

The architecture is sound and sourced. The native bridge compiles against AMD's
real API and produces correctly-exported DLLs. The decision logic — the part
where a mistake means promising a user something their hardware cannot do — is
covered by 118 tests that run without a GPU.

What is missing is everything that needs a Windows machine with a GPU and a
BeamNG install: whether it runs, how fast, how it looks, and whether the two
undocumented BeamNG encodings were guessed correctly. Those are marked as
missing rather than filled in with plausible numbers.

## Licence

This project: MIT ([`LICENSE`](LICENSE)).

The AMD FSR runtime DLLs are redistributed in binary form under AMD's own
licence, reproduced in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). No
AMD source is vendored here; the build fetches it.

Not affiliated with, endorsed by, or supported by BeamNG GmbH or AMD.
