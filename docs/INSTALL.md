# Installation

FSR needs **both halves**. Installing only the mod ZIP does nothing except tell
you the bridge is missing — BeamNG's Lua sandbox cannot load native code, so the
FSR work has to happen in a separate component. See `FEASIBILITY.md` §1.3.

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

Copy `beamng_fsr.zip` into your BeamNG user mods folder, normally:

```
%LOCALAPPDATA%\BeamNG.drive\<version>\mods\
```

Do not unzip it. Enable it in the in-game mod manager if it is not enabled
automatically.

## Step 3 — Install the native bridge

Copy **every file** from the `beamng_fsr_bridge` folder into the directory
containing `BeamNG.drive.exe`, normally:

```
...\Steam\steamapps\common\BeamNG.drive\Bin64\
```

That folder contains:

| File | What it is |
| --- | --- |
| `dxgi.dll` | Proxy stub. Forwards every call to the genuine system `dxgi.dll`. |
| `d3d12.dll` | Proxy stub. Forwards every call to the genuine system `d3d12.dll`. |
| `beamng_fsr_bridge.dll` | The bridge itself. |
| `amd_fidelityfx_loader_dx12.dll` | AMD's FSR loader. |
| `amd_fidelityfx_upscaler_dx12.dll` | AMD's FSR upscaling implementations. |
| `amd_fidelityfx_framegeneration_dx12.dll` | AMD's FSR frame-generation implementations. |

**No BeamNG file is modified, overwritten or replaced.** The two proxies are
loaded ahead of the system DLLs because Windows searches the executable's own
directory first, and both forward everything they receive.

If Windows SmartScreen or your antivirus flags the DLLs: they are unsigned
locally-built binaries placed next to a game executable, which is a pattern
worth being suspicious of. Build them yourself from source (`BUILD.md`) if you
would rather not trust a download.

## Step 4 — Configure

1. Launch BeamNG.
2. Open the UI app manager and add the **AMD FSR** app.
3. Check the **Diagnostics** tab. It should show your GPU, `Direct3D 12`, and a
   bridge version. If it says the bridge is not loaded, see `TROUBLESHOOTING.md`.
4. Turn on **FSR enabled**.
5. Set **Frame Generation** to `Auto`. This is the feature that works best here.
6. Leave the upscaler off unless you have read `LIMITATIONS.md` §1 and §2 and
   accept the trade-offs.

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
