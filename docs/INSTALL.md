# Installation

There are two products. **Install whichever you want; neither needs the other.**

| Product | Folder | Install it for |
| --- | --- | --- |
| AMD FSR Frame Generation | `dist/framegen/` | More displayed frames, plus Anti-Lag 2. The stronger feature here. |
| AMD FSR Upscaling | `dist/upscaling/` | Rendering below display resolution and upscaling back. Read `LIMITATIONS.md` §1 first. |

Each product needs **both of its halves**: the mod ZIP and the bridge folder.
Installing only the mod ZIP does nothing except tell you the bridge is missing —
BeamNG's Lua sandbox cannot load native code, so the FSR work has to happen in a
separate component. See `FEASIBILITY.md` §1.3.

## Installing both

Supported and expected to be common. The two bridge folders contain byte-identical
binaries, so copy either first and then the other; the only new files the second
adds are its own module manifest and its own AMD effect DLL. The two mod ZIPs
likewise share three identical files and otherwise do not overlap.

## Requirements

| | |
| --- | --- |
| OS | Windows 10 or 11 (Windows 11 required for ML frame generation) |
| BeamNG | v0.39 or newer, running the **Direct3D 12** renderer |
| GPU | See `COMPATIBILITY_MATRIX.md`. Analytical FSR covers most DX12 GPUs; ML features are AMD-only. |
| Renderer | **Direct3D 12 only.** Vulkan and D3D11 cannot work — see `LIMITATIONS.md` §3. |

## Step 1 — Confirm the Direct3D 12 renderer

D3D12 is BeamNG's default on Windows since v0.39. To be certain, choose
*Launch (D3D12 renderer)* in the launcher, or add the Steam launch option:

```
-gfx d3d12
```

## Step 2 — Install the BeamNG mod

Copy the product's ZIP into your BeamNG user mods folder, normally:

```
%LOCALAPPDATA%\BeamNG.drive\<version>\mods\
```

- Frame generation: `beamng_fsr_framegen.zip`
- Upscaling: `beamng_fsr_upscaling.zip`

Do not unzip them. Enable them in the in-game mod manager if they are not
enabled automatically.

## Step 3 — Install the native bridge

Copy **everything** inside the product's `bridge` folder — including the
`beamng_fsr\` subfolder — into the directory containing `BeamNG.drive.exe`,
normally:

```
...\Steam\steamapps\common\BeamNG.drive\Bin64\
```

Each bridge folder contains:

| File | What it is | Shared between products |
| --- | --- | --- |
| `dxgi.dll` | Proxy stub; forwards to the genuine system `dxgi.dll` | yes, identical |
| `d3d12.dll` | Proxy stub; forwards to the genuine system `d3d12.dll` | yes, identical |
| `beamng_fsr_bridge.dll` | The bridge runtime | yes, identical |
| `amd_fidelityfx_loader_dx12.dll` | AMD's FSR loader | yes, identical |
| `amd_fidelityfx_framegeneration_dx12.dll` | AMD's frame-generation implementations | frame generation only |
| `amd_fidelityfx_upscaler_dx12.dll` | AMD's upscaling implementations | upscaling only |
| `beamng_fsr\modules\*.module.json` | Declares which product is installed | one per product |

**The manifest is the whole mechanism.** Delete `framegen.module.json` and the
runtime never creates a frame-generation context, never wraps the swapchain and
never reads its settings — while upscaling carries on working. The reverse holds
too.

**No BeamNG file is modified, overwritten or replaced.** The two proxies are
loaded ahead of the system DLLs because Windows searches the executable's own
directory first, and both forward everything they receive.

If Windows SmartScreen or your antivirus flags the DLLs: they are unsigned
locally-built binaries placed next to a game executable, which is a pattern
worth being suspicious of. Build them yourself from source (`BUILD.md`) if you
would rather not trust a download.

## Step 4 — Configure

1. Launch BeamNG.
2. Open the UI app manager and add the app for the product you installed:
   **AMD FSR Frame Generation** and/or **AMD FSR Upscaling**.
3. Each panel shows `active` or `inactive` in its header. Expand
   **Diagnostics** — it should show your GPU, `Direct3D 12` and a bridge version.
   If it says the bridge is not loaded, see `TROUBLESHOOTING.md`.
4. Turn the product on with the switch at the top and leave **Implementation**
   on `Auto`.

Each panel also reports whether the *other* product is installed, so you can
tell at a glance what you have without either depending on the other.

## Step 5 — Verify

The Diagnostics tab should report:

- **Depth source** and **Motion vectors** as something other than "not identified".
- **Rendered FPS** and **Displayed FPS** as separate numbers. With frame
  generation active, displayed should be roughly double rendered. They are never
  combined into one figure, because they do not mean the same thing.

If depth or motion vectors are unidentified, check that BeamNG's *Lighting
Quality* is at least **Low** — BeamNG only generates motion vectors above that
threshold.

## Uninstalling

See `UNINSTALL.md`. It is four file deletions and nothing else.
