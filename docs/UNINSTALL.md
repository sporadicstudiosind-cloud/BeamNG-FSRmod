# Uninstalling

Nothing in this project modifies BeamNG, so removal is only deletion. There is
no installer state, no registry key and no patched file to restore.

Because the two products are independent, there are three useful levels of
removal.

---

## Level 1 — disable one product, keep the other

Delete just that product's manifest from the folder containing
`BeamNG.drive.exe`:

```
beamng_fsr\modules\framegen.module.json     ← disables frame generation
beamng_fsr\modules\upscaling.module.json    ← disables upscaling
```

The shared runtime then never starts that feature: no context is created, its
settings are not read, and its status document stops being written. The other
product carries on untouched. This is the cleanest way to turn one off while
troubleshooting.

## Level 2 — remove one product completely

Delete its manifest (above), its mod ZIP, and its AMD effect DLL:

| Product | Mod ZIP | AMD effect DLL |
| --- | --- | --- |
| Frame generation | `beamng_fsr_framegen.zip` | `amd_fidelityfx_framegeneration_dx12.dll` |
| Upscaling | `beamng_fsr_upscaling.zip` | `amd_fidelityfx_upscaler_dx12.dll` |

Leave the shared files in place — the other product needs them.

## Level 3 — remove everything

Delete these from the folder containing `BeamNG.drive.exe` (normally
`...\Steam\steamapps\common\BeamNG.drive\Bin64\`):

```
dxgi.dll
d3d12.dll
beamng_fsr_bridge.dll
amd_fidelityfx_loader_dx12.dll
amd_fidelityfx_framegeneration_dx12.dll     (if present)
amd_fidelityfx_upscaler_dx12.dll            (if present)
AMD_FSR_SDK_LICENSE.md
THIRD_PARTY_NOTICES.md
LICENSE
README.txt
beamng_fsr\          (folder: manifests, settings, logs, IPC, benchmarks)
```

Deleting `dxgi.dll` and `d3d12.dll` restores stock behaviour immediately —
Windows then resolves both from the system directory as it did before. There is
no "original" to put back, because the originals were never touched.

Then delete the mod ZIPs from:

```
%LOCALAPPDATA%\BeamNG.drive\<version>\mods\
```

## Leftover settings

Each product stores its settings separately in BeamNG's user folder:

```
%LOCALAPPDATA%\BeamNG.drive\<version>\settings\beamng_fsr_framegen.json
%LOCALAPPDATA%\BeamNG.drive\<version>\settings\beamng_fsr_upscaling.json
```

Delete whichever you want a clean slate for. Leaving them is harmless.

## Keeping the mod but disabling everything

Delete only `dxgi.dll`, `d3d12.dll` and `beamng_fsr_bridge.dll`. Both mod
panels then report the bridge as missing and do nothing else. Neither errors,
and nothing else in BeamNG is affected.
