# Uninstalling

Nothing in this project modifies BeamNG, so removal is only deletion. There is
no installer state, no registry key and no patched file to restore.

## Remove the native bridge

Delete these from the folder containing `BeamNG.drive.exe` (normally
`...\Steam\steamapps\common\BeamNG.drive\Bin64\`):

```
dxgi.dll
d3d12.dll
beamng_fsr_bridge.dll
amd_fidelityfx_loader_dx12.dll
amd_fidelityfx_upscaler_dx12.dll
amd_fidelityfx_framegeneration_dx12.dll
AMD_FSR_SDK_LICENSE.md
THIRD_PARTY_NOTICES.md
LICENSE
README.txt
beamng_fsr\        (folder: settings, logs, IPC files, benchmark exports)
```

Deleting `dxgi.dll` and `d3d12.dll` restores the stock behaviour immediately —
Windows then resolves both from the system directory as it did before. There is
no "original" to put back, because the originals were never touched.

## Remove the BeamNG mod

Delete `beamng_fsr.zip` from:

```
%LOCALAPPDATA%\BeamNG.drive\<version>\mods\
```

## Remove leftover settings

The mod stores its settings in BeamNG's user folder:

```
%LOCALAPPDATA%\BeamNG.drive\<version>\settings\beamng_fsr_settings.json
```

Delete it if you want a clean slate. Leaving it is harmless.

## Partial removal

To keep the mod but disable FSR entirely, delete only `dxgi.dll`, `d3d12.dll`
and `beamng_fsr_bridge.dll`. The mod detects the missing bridge and reports it
in the UI; it does not error and does not affect anything else.
