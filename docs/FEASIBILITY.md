# Feasibility Analysis — AMD FSR in BeamNG.drive

**Status:** Phase 1 deliverable. Every load-bearing claim below is sourced.
**Research date:** 2026-08-09
**Subject versions:** BeamNG.drive v0.39.2 (Windows) · AMD FSR SDK 2.3.0 "Redstone"

---

## 0. Executive verdict

| Question | Answer |
| --- | --- |
| Can FSR 3.1 / FSR 4 / FSR Frame Generation be implemented inside a normal BeamNG mod ZIP (Lua/HTML/JS + PostFX)? | **No. Categorically impossible.** |
| Is there a technically valid way to do it at all? | **Yes** — a native D3D12/DXGI interception layer running in the BeamNG process, loaded via the Windows DLL search order. |
| Does that layer require reverse-engineering BeamNG? | **No**, if it is restricted to hooking Microsoft-documented APIs only. This project enforces that as a hard rule (§7). |
| Can FSR **Frame Generation** be made to work correctly this way? | **Yes**, with cooperation from a Lua/PostFX mod half. This is the strongest feature. |
| Can FSR **upscaling** be made to work *correctly*? | **Partially.** It runs, but without engine-side camera jitter it is operating outside AMD's documented requirements and loses its primary quality advantage. Documented as a first-class limitation (§6.3). |
| Can BeamNG use AMD's driver-side FSR 4 upgrade? | **No.** AMD explicitly excludes third-party integration methods (§4.4). |
| Can the AMD DLLs be redistributed with the mod? | **Yes, binaries only**, with the required notices (§8). |

The honest one-line summary: **BeamNG's mod API cannot reach the renderer, so the only real path is a native DX12 bridge; frame generation is genuinely achievable through it, upscaling is achievable but compromised, and no amount of Lua can change that.**

---

## 1. What BeamNG.drive actually exposes

### 1.1 Graphics APIs

BeamNG ships three renderers on Windows: **Direct3D 11**, **Direct3D 12**, and **Vulkan** (Vulkan is also the native Linux renderer).

- **Direct3D 12 is the default renderer on Windows as of v0.39.** ([BeamNG docs — Direct3D 12 Renderer](https://documentation.beamng.com/support/troubleshooting/direct3d12/))
- D3D12 path supports feature levels `D3D_FEATURE_LEVEL_11_0` … `D3D_FEATURE_LEVEL_12_2`, requires **Shader Model 6.0+** for SM6 effects, Resource Binding Tier 2+, Resource Heap Tier 1+. (ibid.)
- D3D11 is described by BeamNG as "an obsolete fallback renderer". (ibid.)
- Renderer is selectable via launcher or `-gfx d3d12` / `-gfx dx11`. (ibid.)

**Consequence:** the default renderer is the one API that AMD's current FSR actually supports (§4.2). This alignment is what makes the project viable at all.

### 1.2 The Lua modding surface

BeamNG mods are Lua + HTML/JS. Extensions live in `lua/ge/extensions/`, `lua/vehicle/extensions/`, `lua/common/extensions/`, are loaded with `extensions.load("mod_name")`, and receive lifecycle hooks including `onExtensionLoaded`, `onUpdate` (once per graphics frame), `onGuiUpdate`, `onSerialize`. ([BeamNG docs — Extensions](https://documentation.beamng.com/modding/programming/extensions/))

BeamNG's own documentation states plainly that "a detailed API documentation is outside the scope of this documentation" and directs modders to read the shipped Lua source. ([BeamNG docs — Programming](https://documentation.beamng.com/modding/programming/))

### 1.3 **The blocker: native code cannot be loaded from Lua**

BeamNG's Lua VM is sandboxed. Attempting `ffi.load` or `package.loadlib` yields:

```
Sandbox: ffi.load denied when calling package.loadlib
```

A BeamNG developer (`tdev`) responded on the official forums:

> "Our game is sandboxed for security reasons, so this will not be possible in a proper way, sorry"

([BeamNG forums — "Loading lua C dll?"](https://www.beamng.com/threads/loading-lua-c-dll.88622/))

**This single fact eliminates the entire class of "Lua mod calls a native FSR DLL" architectures.** It is the most important finding in this document. Any project claiming to implement FSR 3/4 as a pure BeamNG mod ZIP is either misinformed or lying.

### 1.4 The PostFX surface — what it *can* do

BeamNG inherits a Torque3D-derived `PostEffect` system that **is** exposed to Lua and **is** genuinely powerful. ([BeamNG docs — Post Process Effects](https://documentation.beamng.com/modding/postfx/))

Available to mods:

| Capability | Detail |
| --- | --- |
| Object types | `ShaderData` (with `DXVertexShaderFile`, `DXPixelShaderFile`, `pixVersion`), `GFXStateBlockData`, `PostEffect` |
| Discovery/mutation | `scenetree.findObject()`, `enable()`, `disable()`, `setField()` |
| Custom shaders | Custom HLSL fullscreen effects are supported ("an advanced workflow") |
| Depth | `#prepass[Depth]` — device depth |
| Normals/roughness | `#prepass[RT0]` |
| Specular | `#prepass[RT1]` |
| **Motion vectors** | **`#VelocityBuffer[RT0]`** |
| Other targets | `#glowbuffer[RT0]`, `#ssaoMask[RT0]`, `#sssMask[RT0]` |
| Special tokens | `$backBuffer`, `$inTex`, `$outTex`, `$skyCameraVolume` |
| Auto-bound constants | `targetSize`, `projParams`, `eyePosWorld`, `sunDirection`, `exposure`, `accumTime`, `deltaTime` |
| Custom constants | `setShaderConst("$name", value)` |

Corroborating the velocity buffer's existence: BeamNG's motion blur requires motion vectors, and community documentation notes motion vectors are generated only when lighting quality is at least "low". The community "Temporal Anti Aliasing (Standalone)" mod is built on exactly this surface.

### 1.5 The PostFX surface — what it *cannot* do

This is where a naive design fails. PostFX **cannot**:

1. **Run compute shaders with UAVs.** It is a fullscreen *pixel shader* system (`DXPixelShaderFile`). FSR 3.1/4 are compute workloads (`CS_6_6` for FSR 4 — §4.2).
2. **Load or call the AMD FSR DLLs.** Requires native code — blocked by §1.3.
3. **Change the 3D scene's render resolution.** PostFX runs *after* the scene is rendered, at the existing target size. Temporal upscaling fundamentally requires rendering the scene at a lower resolution.
4. **Apply sub-pixel jitter to the camera projection matrix.** Jitter must be applied *before* geometry rasterisation. There is no documented Lua hook that injects a per-frame jitter offset into the scene projection matrix.
5. **Insert itself into presentation.** Frame generation requires control of the swapchain and `Present`. PostFX has no such reach.
6. **Provide `ID3D12Device` / `ID3D12GraphicsCommandList` handles.** The FSR API requires both.

**Conclusion for §1:** BeamNG's mod API gives you *pixels* (color, depth, velocity) but not *control* (compute, resolution, jitter, presentation) and not *native code*. FSR needs all six.

---

## 2. Architectures evaluated

| # | Architecture | Verdict | Reason |
| --- | --- | --- | --- |
| A | Pure Lua + PostFX HLSL implementing FSR | **Rejected** | Cannot do compute/UAV, cannot change render resolution, cannot jitter. At best one could hand-port **FSR 1** (spatial EASU/RCAS, a pixel-shader algorithm) — but FSR 1 is not FSR 3/4 and the user explicitly asked for temporal FSR + FG. |
| B | Lua `ffi.load` a native companion DLL | **Rejected** | Blocked by BeamNG's sandbox — confirmed by a BeamNG developer (§1.3). |
| C | External process + shared-surface capture | **Rejected** | Requires cross-process GPU sharing of BeamNG-internal depth/MV textures, which BeamNG does not export. A capture-only variant could only see the final composited frame — no depth, no MVs, so FSR cannot run. |
| D | Modify/replace BeamNG binaries or patch memory | **Rejected on principle** | Would constitute reverse engineering/modification of the Software, which the BeamNG EULA prohibits (§7). Also breaks on every update. This project will not do it. |
| E | **Native D3D12/DXGI interception layer via DLL search order + cooperating Lua/PostFX mod half** | **Accepted** | Only remaining path. Uses exclusively Microsoft-documented APIs on the native side and exclusively documented modding APIs on the BeamNG side. |

### 2.1 Precedent for (E)

ReShade — which works by placing a proxy `dxgi.dll`/`d3d11.dll` next to the game executable — is confirmed working with BeamNG.drive by the community (with *UI Hardware Acceleration* disabled, a detail this project must handle; see §6.5). This establishes that the DLL-search-order mechanism functions with BeamNG and that BeamNG ships no anti-tamper that blocks it.

---

## 3. The chosen architecture, in one diagram

```
┌─────────────────────── BeamNG.drive process ────────────────────────┐
│                                                                      │
│  BeamNG engine (opaque, never touched)                              │
│      │                                                               │
│      │ renders scene → #prepass[Depth], #VelocityBuffer[RT0]        │
│      ▼                                                               │
│  ┌────────────────────────────────────────────┐                     │
│  │ MOD HALF  (normal BeamNG mod ZIP)          │                     │
│  │  • Lua extension  (documented API)         │                     │
│  │  • PostEffect + custom HLSL "beacon" pass  │───┐                 │
│  │    packs depth+MV into a uniquely-shaped   │   │ GPU texture     │
│  │    render target the native half can find  │   │ (identified by  │
│  │  • Settings UI, config persistence         │   │  resource desc) │
│  │  • Camera near/far/FOV → IPC               │   │                 │
│  └────────────────────────────────────────────┘   │                 │
│      │ file IPC (JSON in BeamNG userfolder)       │                 │
│      ▼                                             ▼                 │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │ NATIVE HALF  (dxgi.dll proxy — Microsoft APIs only)          │   │
│  │  • forwards every DXGI export to the real system dxgi.dll    │   │
│  │  • wraps IDXGIFactory::CreateSwapChainForHwnd                │   │
│  │  • tracks ID3D12Device resource creation (desc only)         │   │
│  │  • FfxLoader → amd_fidelityfx_loader_dx12.dll                │   │
│  │  • FSR Upscale context   ─┐                                  │   │
│  │  • FSR FrameGen context   ├─ AMD signed DLLs                 │   │
│  │  • FG Swapchain wrapper  ─┘                                  │   │
│  │  • Anti-Lag 2 (AMD driver ext, AMD GPUs only)                │   │
│  └──────────────────────────────────────────────────────────────┘   │
│      │                                                               │
│      ▼  Present                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

### 3.1 Why the "beacon" is the key idea

A generic D3D12 interceptor has no idea which of the ~hundreds of textures a frame touches is *the* depth buffer or *the* velocity buffer. Heuristic classification (by format + size + flags) is fragile and version-dependent.

Instead, the **mod half publishes the data itself**: a `PostEffect` writes depth and motion vectors into a mod-owned render target whose *resource description is deliberately unique* (dimensions and format chosen so no engine resource collides). The native half then identifies that texture by an exact `D3D12_RESOURCE_DESC` signature agreed over IPC.

This is robust across BeamNG updates in a way heuristics are not, because the identifying signature is **created by our own mod**, not inferred from the engine. It also uses only documented APIs on both sides.

Heuristic classification is still implemented as a **fallback** (`HeuristicFrameInputProvider`) for when the mod half is absent, and is clearly reported as lower-confidence in diagnostics.

---

## 4. The current AMD FSR landscape (verified against SDK 2.3.0 source tree)

Verified by cloning `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK` and reading `readme.md`, `docs/license.md`, the public headers, and `Kits/FidelityFX/signedbin/`.

### 4.1 Component inventory — AMD FSR SDK 2.3.0 "Redstone"

| Technique | Version | Kind | Header constant |
| --- | --- | --- | --- |
| FidelityFX Super Resolution (Temporal) | **2.3.4** | analytical upscaler (legacy) | — |
| FidelityFX Super Resolution (Upscaler) | **3.1.5** | analytical upscaler | — |
| **FSR Upscaling (ML-Upscaler)** | **4.1.1** | ML upscaler | `FFX_UPSCALER_VERSION_{MAJOR,MINOR,PATCH} = 4,1,1` |
| FidelityFX SR Frame Generation | **3.1.6** | analytical FG | — |
| FidelityFX SR Frame Generation SwapChain | **3.1.7** | DXGI swapchain replacement | `FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION_* = 3,1,7` |
| **FSR Frame Generation (ML)** | **4.0.1** | ML FG | `FFX_FRAMEGENERATION_VERSION_* = 4,0,1` |
| FSR Ray Regeneration (ML denoiser) | 1.2.0 | ML denoiser | out of scope |
| FSR Radiance Caching | Preview | ML illumination cache | out of scope |

Note the naming trap the brief warned about: the **upscaler** is at 4.1.1 while **frame generation** is at 4.0.1 — they version independently. "FSR 4" is not one number. The UI in this project therefore shows AMD's marketing name *and* the runtime-reported implementation version side by side.

### 4.2 Requirements matrix (from AMD GPUOpen product pages)

| Technique | GPUs | Shader Model | API | OS |
| --- | --- | --- | --- | --- |
| FSR Upscaling 4.1.1 (ML) | "AMD Radeon™ RX 9000 and 7000 Series discrete GPUs" | **SM 6.6** | **DirectX 12** | Win 10 / 11 |
| FSR 3.1.5 / 2.3.4 (analytical) | "AMD Radeon™ RX 500 Series GPUs and above (**or equivalent from other GPU vendors**)" | **SM 6.2** | **DirectX 12** | Win 10 / 11 |
| FSR Frame Generation 4.0.x (ML) | "AMD Radeon™ RX 9000 Series graphics cards and above" | HLSL **CS_6_6**, needs **DX12 Agility SDK 1.4.9+** | DirectX 12 | **Windows 11 only** |
| FSR Frame Generation 3.1.x (analytical) | "GPU supporting Shader Model 6.2 or above" | SM 6.2 | DirectX 12 | Win 11 **and** Win 10 |

Sources: [FSR Upscaling](https://gpuopen.com/amd-fsr-upscaling/), [FSR Frame Generation](https://gpuopen.com/amd-fsr-framegeneration/), [FSR 4](https://gpuopen.com/fidelityfx-super-resolution-4/), [FSR Redstone for developers](https://gpuopen.com/learn/amd-fsr-redstone-developers-neural-rendering/).

Two findings worth calling out because they contradict widely-repeated older information:

- **ML upscaling is no longer RDNA 4 exclusive.** FSR Upscaling 4.1.1 adds RX 7000 (RDNA 3) support. Anything asserting "FSR 4 requires RDNA 4" is describing the 2025 launch state, not the current state.
- **ML frame generation *is* still RX 9000+ and Windows 11 only** — a *stricter* requirement than ML upscaling. Conflating the two would produce a wrong capability matrix. This project models them as separate capability bits.

### 4.3 **Vulkan: not available**

Three independent confirmations that the DX12-only conclusion is correct for SDK 2.3.0:

1. SDK `readme.md` → Known issues: **"Vulkan is currently not supported in SDK"**.
2. `Kits/FidelityFX/signedbin/` contains **only** `*_dx12.dll` — `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`, `amd_fidelityfx_framegeneration_dx12.dll`, `amd_fidelityfx_denoiser_dx12.dll`, `amd_fidelityfx_radiancecache_dx12.dll`. There is no Vulkan loader binary.
3. Every GPUOpen requirements page lists DirectX 12 and does not list Vulkan.

There is a *residual* Vulkan surface in the API headers (`FFX_API_BACKEND_ID_VK`, and `FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN_VK` which is explicitly commented **"deprecated VK frame gen swapchain"**), but no shipping backend binary.

**Therefore: BeamNG's Vulkan renderer cannot be used.** The mod must detect the active renderer and cleanly disable itself under Vulkan and D3D11. This is implemented, not hand-waved.

### 4.4 **AMD's driver-side FSR 4 upgrade: BeamNG cannot qualify**

AMD's requirement for the Adrenalin driver-level FSR upgrade is a *native, in-game* FSR 3.1 (upscaling) or 3.1.4 (frame generation) integration on DirectX 12 using **the signed FSR 3.1 DLL**, integrated per AMD's developer guidelines. AMD explicitly states it is **not** compatible with Vulkan titles or with "non-standard methods of integration of FSR 3.1 into their games, such as using **third-party plug-ins**".

BeamNG.drive has no FSR integration at all, and this project *is* a third-party layer. **BeamNG cannot qualify for the driver upgrade path, now or via this mod.** Native integration through the FidelityFX API (what this project does) is the only route to FSR 4 in BeamNG.

### 4.5 The DLL architecture and runtime version selection (real, verified)

Since SDK 2.0.0 the effects are split behind a thin loader:

```
amd_fidelityfx_loader_dx12.dll          26 KB   ← the only thing you link/LoadLibrary
amd_fidelityfx_upscaler_dx12.dll        28 MB   ← FSR Upscaling 4.x + FSR 3.1.5 + FSR 2.3.4
amd_fidelityfx_framegeneration_dx12.dll 40 MB   ← FG 4.0.1 + FG 3.1.6 + FG Swapchain 3.1.7
amd_fidelityfx_denoiser_dx12.dll        14 MB   ← Ray Regeneration
amd_fidelityfx_radiancecache_dx12.dll  799 KB   ← Radiance Caching (preview)
```

The five entry points are resolved by name from the loader (`ffx_api_loader.h`, `ffxLoadFunctions`): `ffxCreateContext`, `ffxDestroyContext`, `ffxConfigure`, `ffxQuery`, `ffxDispatch`.

**Version selection is a first-class AMD feature, not a hack.** `ffxQuery(nullptr, ffxQueryDescGetVersions{createDescType, device, outputCount, versionIds, versionNames})` enumerates every implementation the installed DLLs can provide *for the given device*, returning both opaque IDs and display names. Passing `ffxOverrideVersion{versionId}` in the create-context chain pins a specific one.

This is exactly what the brief asked for ("multiple selectable FSR versions"), and it means:

- The version list is **discovered at runtime**, not hardcoded. Dropping in a newer AMD SDK adds versions with no code change.
- The list is **already hardware-filtered by AMD**, because the query takes the `ID3D12Device`. An RX 6000 will simply not be offered ML upscaling.
- The UI must therefore be **populated from the query**, never from a static list. This project does that.

### 4.6 Anti-Lag 2

`ffx_antilag2_dx12.h` is a header-only MIT shim. It resolves `AmdExtD3DCreateInterface` from the AMD driver module and drives `IAmdExtAntiLagApi::UpdateAntiLagState`, exposing `Initialize`, `DeInitialize`, `Update`, `MarkEndOfFrameRendering`, `SetFrameGenFrameType`. It is an **AMD-driver-only** facility: on NVIDIA/Intel it simply fails to initialise, and the option must be hidden. AMD recommends pairing it with frame generation.

---

## 5. Mapping BeamNG's data onto FSR's required inputs

`ffxDispatchDescUpscale` and `ffxDispatchDescFrameGenerationPrepareV2` require the following. This table is the real engineering core of the project.

| FSR input | BeamNG source | Obtainable? | Notes |
| --- | --- | --- | --- |
| `color` (render res) | scene color / `$backBuffer` | ✅ | via beacon pass or swapchain |
| `depth` (render res) | `#prepass[Depth]` | ✅ | needs convention conversion — §5.1 |
| `motionVectors` | `#VelocityBuffer[RT0]` | ✅ | needs scale/convention conversion — §5.2 |
| `output` (display res) | mod-allocated | ✅ | native half allocates |
| `exposure` | `exposure` shader constant | ⚠️ | scalar available to PostFX; a 1×1 texture must be produced by the beacon pass. Otherwise use `FFX_UPSCALE_ENABLE_AUTO_EXPOSURE`. |
| `jitterOffset` | — | ❌ | **not controllable** — §6.3 |
| `renderSize` / `upscaleSize` | — | ⚠️ | render size = whatever BeamNG is rendering at; not independently controllable — §6.4 |
| `cameraNear` / `cameraFar` / `cameraFovAngleVertical` | Lua camera API | ✅ | pushed over IPC each frame |
| `frameTimeDelta` | `deltaTime` constant / native timing | ✅ | |
| `reset` | level load / camera cut, via Lua hooks | ✅ | |
| `reactive` / `transparencyAndComposition` | — | ⚠️ | optional; not provided. FSR runs without them at some quality cost (particles/transparency ghosting). |
| `viewSpaceToMetersFactor` | BeamNG world units are metres | ✅ | 1.0 |

### 5.1 Depth convention

Must be determined at runtime, not assumed. The relevant FSR flags are `FFX_UPSCALE_ENABLE_DEPTH_INVERTED` (reversed-Z, depth 1→0) and `FFX_UPSCALE_ENABLE_DEPTH_INFINITE`.

BeamNG exposes a `projParams` auto-constant to PostFX shaders; the beacon shader can therefore *derive and publish* the actual convention rather than guessing. Until verified on a real install, this project:
- defaults to **reversed-Z assumed** (the modern default and what `projParams` inspection is expected to confirm),
- exposes an explicit override in the UI (`Depth convention: Auto / Reversed-Z / Standard`),
- logs the detected convention at init,
- and marks this cell **UNVERIFIED** in the compatibility matrix until confirmed against a real BeamNG install.

This is deliberately not asserted as fact. See `docs/LIMITATIONS.md` §"Unverified against a live install".

### 5.2 Motion vector convention

`ffxDispatchDescUpscale::motionVectorScale` converts BeamNG's velocity encoding into FSR's expected space. FSR wants pixel-space MVs at render resolution (or UV-space scaled by `renderSize`). BeamNG's `#VelocityBuffer` encoding (UV vs pixel, sign, Y direction) is **not documented** and must be calibrated. The project therefore:
- exposes `motionVectorScale` X/Y and a Y-flip toggle in Advanced settings,
- ships a **MV calibration debug view** (`FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW`) so a user on real hardware can confirm in one minute,
- defaults to UV-space with Y-flip, and logs the assumption loudly.

Guessing silently here would produce exactly the "invented compatibility" the brief forbids.

### 5.3 Object motion vs camera motion

BeamNG's velocity buffer is engine-generated for motion blur, which means it is a **true per-pixel velocity buffer including object motion**, not a camera-only reprojection. This matters enormously for BeamNG specifically: vehicles, traffic, deforming bodies, wheels, and particles all move independently of the camera. This project does **not** substitute camera reprojection for object motion — if the velocity buffer cannot be located, the upscaler is disabled rather than run with wrong data.

---

## 6. Honest limitations

### 6.1 Vulkan and D3D11 are unsupported — permanently, not temporarily
See §4.3. The mod detects the renderer and disables cleanly with a UI explanation.

### 6.2 ML Frame Generation requires Windows 11 + RX 9000
Stricter than ML upscaling (§4.2). A Windows 10 user on an RX 9070 gets ML upscaling but **not** ML FG. The capability model encodes this.

### 6.3 **Camera jitter cannot be applied — the fundamental limitation**

FSR's temporal upscaler reconstructs sub-pixel detail by accumulating frames rendered with a jittered projection matrix. It exposes `ffxQueryDescUpscaleGetJitterPhaseCount` / `GetJitterOffset` precisely so the *game* can apply that jitter before rasterising.

**No documented BeamNG modding API can inject a per-frame jitter offset into the scene projection matrix.** PostFX runs after rasterisation; Lua has no such hook.

Consequences, stated plainly:
- FSR upscaling will still *run* and still temporally accumulate, but without jitter it cannot recover sub-pixel information. Output is materially worse than a native FSR integration — closer to a good temporal upsample than to true FSR quality.
- This is **outside AMD's documented usage**. It is not a bug in this project and it is not fixable from the mod side. Only BeamNG can fix it, by either integrating FSR natively or exposing a projection-jitter hook.
- The UI states this explicitly wherever upscaling is enabled. It is not buried in a README.
- If BeamNG's own TAA already jitters the camera, the phase sequence is BeamNG's and does not match FSR's expected Halton sequence; feeding FSR a `jitterOffset` we did not choose is not equivalent and is not attempted.

**Frame generation is not affected by this** to anything like the same degree — `jitterOffset` in the FG prepare dispatch describes the input frames' jitter, and zero is a coherent value for an unjittered renderer. This is why FG is the stronger feature here.

### 6.4 Render resolution is not independently controllable
The native layer cannot make BeamNG render its 3D scene at a different resolution from the swapchain. Two usable modes result:

- **FG-only mode (recommended):** BeamNG renders at native; the layer adds interpolated frames. Fully coherent.
- **Upscale mode:** the user lowers BeamNG's own render/window resolution and the layer upscales to display resolution at present time. Works, but BeamNG's UI is also rendered at the lower resolution and is then upscaled — a real quality cost, disclosed in the UI.

A true "render 1440p → present 4K with native UI" split requires engine cooperation.

### 6.5 BeamNG UI Hardware Acceleration
Community experience with ReShade shows BeamNG's CEF-based UI conflicts with DXGI-level interception when *UI Hardware Acceleration* is enabled (the main menu becomes invisible). The installer and the mod's first-run check therefore detect this setting and warn. It also affects UI compositing during frame generation: with HW accel on, the UI is not part of the backbuffer in a way the FG swapchain's UI-composition path can cleanly use.

### 6.6 Anti-cheat / multiplayer
BeamNG ships no anti-tamper. Third-party multiplayer mods are outside this project's scope and support; a DXGI proxy may violate *their* rules. The mod does not attempt to hide itself and logs its presence.

### 6.7 Not verified on real hardware
No BeamNG installation, no AMD GPU, and no Windows machine were available in the build environment. **No performance number, image-quality judgement, or hardware compatibility result in this repository is claimed as measured.** The compatibility matrix marks every hardware row `NOT TESTED` and the test plan is written to be executed by someone with the hardware. Fabricating those results would be worse than not having them.

---

## 7. Legal analysis

### 7.1 BeamNG EULA
The EULA prohibits, in relevant part, that you "in whole or in part, reverse engineer, merge, translate, disassemble or decompile the Software", while carving out that **"Modding of the Software's content files is not reverse engineering"**. It also forbids exploiting the Software for commercial purposes. ([BeamNG EULA](https://beamng.com/game/support/policies/eula/))

Applied to this project:

| Action | Taken? | Assessment |
| --- | --- | --- |
| Decompile/disassemble BeamNG binaries | **No — forbidden by project rule** | Would violate the EULA |
| Signature-scan / pattern-match BeamNG code | **No — forbidden by project rule** | Reverse engineering in substance |
| Patch BeamNG memory or hook BeamNG functions | **No — forbidden by project rule** | Reverse engineering in substance |
| Modify, replace or overwrite any BeamNG file | **No** | Installer writes only to its own directory and the user folder |
| Hook Microsoft's DXGI/D3D12 (documented public APIs) | **Yes** | Not reverse engineering of BeamNG; it is implementing a Microsoft-specified interface |
| Content modding via the documented Lua/PostFX API | **Yes** | Explicitly carved out as permitted |
| Commercial exploitation | **No** | Project is free and non-commercial |

**Honest residual risk:** injecting *any* DLL into the process is not something BeamNG has explicitly blessed. This project's position is that the EULA's prohibition targets reverse engineering and modification of the Software, neither of which occurs here — but that is an interpretation, not a permission slip. It is documented as such in `docs/LEGAL.md`, the installer surfaces it, and the feature is opt-in. If BeamNG objects, the correct response is to stop distributing it, not to argue.

**Hard project rule (enforced in code review and in `docs/ARCHITECTURE.md`):**
> The native component may hook **only** Microsoft-provided APIs. No BeamNG symbol, offset, signature, vtable or binary may be referenced, scanned, patched or depended upon.

This rule is also what makes the mod survive BeamNG updates: there is nothing version-specific to break.

### 7.2 AMD FSR SDK licensing — and a genuine conflict in AMD's own terms

There are **two conflicting licences** in the AMD SDK, and this must not be glossed over:

- `docs/license.md` opens with: *"The following license applies to all files except as noted below"* and grants permission "to install, reproduce, copy and distribute copies of the Software, **in binary form only**", with "No reverse engineering, decompilation, or disassembly", and requires reproducing the notices. The "noted below" exception list covers samples, docs and tools — it **does not list the public API headers**.
- Yet `Kits/FidelityFX/api/include/ffx_api.h`, `ffx_api_types.h`, `ffx_api_loader.h`, `dx12/ffx_api_dx12.h`, `upscalers/include/ffx_upscale.h`, `framegeneration/include/ffx_framegeneration.h`, `framegeneration/include/dx12/ffx_api_framegeneration_dx12.h` and `OpenSource/amd/antilag2/ffx_antilag2_dx12.h` each carry an **inline MIT grant** ("...to deal in the Software without restriction...").

**Resolution adopted (the conservative reading):** treat the blanket binary-only licence as governing redistribution of the SDK tree, and rely on the per-file MIT notices only for *use*, not for *redistribution*. Concretely:

- ✅ The signed AMD DLLs **are** redistributed with the release package (explicitly permitted, "binary form"), with `docs/license.md` reproduced verbatim in `THIRD_PARTY_NOTICES.md`.
- ❌ **No AMD header or source file is vendored into this repository.** The build fetches the SDK via `tools/fetch_fsr_sdk.py`. This costs one build step and removes the ambiguity entirely.
- ❌ No reverse engineering, decompilation or disassembly of the AMD DLLs, and none is needed — the API is public.

This is documented in `docs/LEGAL.md` so a reader can disagree with the interpretation and see exactly what it rests on.

---

## 8. What this project therefore delivers

| Deliverable | Status |
| --- | --- |
| Sourced feasibility analysis (this document) | ✅ |
| Native D3D12 bridge: DXGI proxy, resource tracking, FSR upscaler, FSR frame generation, FG swapchain, Anti-Lag 2 | ✅ implemented, cross-compiled |
| Runtime FSR **version discovery + selection** via AMD's own `ffxQuery`/`ffxOverrideVersion` | ✅ |
| GPU/API/OS/shader-model capability detection driving UI availability | ✅ |
| BeamNG mod half: Lua extension, PostFX beacon, settings UI, diagnostics, persistence | ✅ |
| Failsafe: never crash BeamNG; disable and continue | ✅ |
| Test suite for all platform-independent logic (runs in CI on Linux) | ✅ |
| Hardware compatibility matrix | ✅ (all rows honestly marked NOT TESTED) |
| Measured performance/IQ results | ❌ **Not produced — no hardware.** Test plan provided instead. |

---

## 9. Self-critique of this analysis

Applying the brief's twelve questions:

1. *Compatible with real BeamNG architecture?* — Yes: D3D12 default renderer, real PostFX target names, real sandbox restriction.
2. *Are these APIs real?* — FSR API read from the actual SDK headers; BeamNG APIs from official docs. Nothing invented.
3. *Is the FSR API current?* — SDK 2.3.0, the current "Redstone" release, © 2022-2026.
4. *GPU compatibility verified?* — From AMD's own product pages, including the RDNA 3 ML-upscaling expansion that supersedes older claims.
5. *Graphics API supported?* — DX12 only, triple-confirmed; Vulkan explicitly excluded.
6. *Undocumented assumptions?* — Three, all flagged and made user-overridable: depth convention (§5.1), MV encoding (§5.2), beacon resource identification robustness (§3.1).
7. *Modifying BeamNG files?* — No. Hard rule.
8. *Introducing latency?* — Yes, frame generation inherently does. Native vs displayed FPS are reported separately and never conflated (§ diagnostics).
9. *Could it crash the game?* — Possible in principle for any in-process layer. Mitigated by exception isolation, a safe-mode flag file, and pass-through-on-any-failure. Documented, not denied.
10. *Cleaner architecture?* — Only if BeamNG exposes a native plugin API or integrates FSR itself. Both noted in `docs/FUTURE_WORK.md`.
11. *Newer AMD SDK?* — 2.3.0 is current; the version manager is designed so newer DLLs work without recompiling.
12. *Better official BeamNG mechanism?* — None exists today. Searched and documented.

**The weakest part of this analysis** is §5.1/§5.2: BeamNG's exact depth and velocity encodings are undocumented and could not be verified without an installation. They are therefore treated as calibration parameters with debug tooling, not as facts.
