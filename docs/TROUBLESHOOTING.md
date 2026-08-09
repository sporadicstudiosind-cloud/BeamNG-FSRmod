# Troubleshooting

Start with the log: `Bin64\beamng_fsr\bridge.log`. The previous session is kept
as `bridge.log.1`. Every failure writes a block naming the reason, the required
feature, what was detected, and the fallback taken.

---

## "Native bridge not loaded"

The mod is running but cannot see the bridge.

0. **Check you copied the `beamng_fsr\` subfolder too.** The bridge folder
   contains `beamng_fsr\modules\<product>.module.json`, and without it the
   runtime does not start that product at all — see the next entry.

1. **Check the DLLs are in the right folder.** They go beside
   `BeamNG.drive.exe` (usually `Bin64\`), not in the mods folder and not in the
   game root.
2. **Check the renderer.** Launch with `-gfx d3d12`. Under Vulkan the `d3d12.dll`
   proxy is never loaded, so the bridge never initialises.
3. **Check `bridge.log` exists.** If it does not, neither proxy was loaded at
   all — the files are in the wrong place or blocked by antivirus.
4. **Check for a 32-bit/64-bit mismatch.** The DLLs must be x64.

## BeamNG's main menu is invisible

Turn off **UI Hardware Acceleration** in BeamNG's options. BeamNG's CEF-based
UI conflicts with DXGI-level interception when it is enabled — the same
behaviour reported with ReShade. See `LIMITATIONS.md` §7.

## One product works and the other says "not installed on the bridge side"

The runtime found one manifest but not the other. Copy the missing product's
`bridge\beamng_fsr\modules\` folder over again. The two products deliberately
do not enable each other, so a missing manifest disables exactly one of them.

## "Safe mode is active"

The previous session did not shut down cleanly, so the bridge started with
everything disabled. This is deliberate: repeating whatever crashed is worse
than starting off.

1. Read `bridge.log.1` — that is the session that died.
2. Fix or report what you find.
3. Click **Clear safe mode** in the Advanced tab.

If safe mode returns every launch, the bridge is crashing during startup. Try
**Force compatibility mode** before clearing it.

## "FSR needs depth and motion vectors and they are not available"

FSR will not run on substituted data, so it stops instead.

1. **Set Lighting Quality to Low or higher.** BeamNG only generates motion
   vectors above that threshold. This is the most common cause.
2. **Check the product's mod ZIP is enabled** in BeamNG's mod manager. Without
   it the beacon does not exist and the bridge falls back to heuristics.
3. **Check the log for "beacon"** entries. If the PostEffect failed to create,
   the error is recorded verbatim.
4. **Check for "an interface this build does not wrap"** in the log — see
   `LIMITATIONS.md` §4.

## No FSR options are selectable

Open Diagnostics and read the reason beside each disabled option. The bridge
never hides an option without saying why. Common answers:

| Reason mentions | What it means |
| --- | --- |
| "Vulkan" | Restart with the Direct3D 12 renderer. |
| "Shader Model 6.6" | Your GPU or driver does not report SM 6.6. Update the driver. |
| "RX 9000" / "RX 7000" | AMD does not document this ML feature for your GPU. Analytical FSR should still be offered. |
| "Windows 11" | ML frame generation is Windows 11 only. Analytical frame generation works on Windows 10. |
| "no upscaling provider" | AMD's runtime itself reports nothing usable for this GPU and driver — this overrides everything else. |

## "could not load amd_fidelityfx_loader_dx12.dll"

The AMD runtime is not beside the bridge. Copy the three
`amd_fidelityfx_*_dx12.dll` files from the AMD FSR SDK's
`Kits/FidelityFX/signedbin` folder into the same directory as
`beamng_fsr_bridge.dll`, or reinstall that product's bridge folder.

## Frame generation is on but the frame rate does not change

Check **Interpolation paused** on the Frame Generation tab. Interpolation is
suppressed while paused, loading, in a menu, in a replay, in photo mode, when
the window is occluded, and for a few frames after any of those — because
interpolating across those boundaries produces visible artefacts.

Also check that **Rendered FPS** and **Displayed FPS** are both non-zero in
Diagnostics. If displayed equals rendered, interpolation is not running.

## The image is smeared, ghosted, or the motion looks wrong

Almost always the motion-vector encoding, which BeamNG does not document.

1. Turn on **Advanced → FSR debug view**. AMD's visualisation makes the
   direction and scale obvious.
2. Toggle **Flip motion vector Y**.
3. Toggle **Motion vectors are UV space**.
4. Adjust **MV scale X/Y**.

Please report what worked — these defaults are unverified assumptions
(`LIMITATIONS.md` §5) and a confirmed value would let them become facts.

## Everything looks soft with upscaling on

Expected. See `LIMITATIONS.md` §1: BeamNG exposes no camera-jitter hook, so FSR
runs without the sub-pixel jitter its upscaler is designed around. No setting
fixes this. Use frame generation instead, or a gentler quality preset.

## Reporting a problem

Advanced → **Export report** in the panel for the product that is misbehaving.
It writes `beamng_fsr_framegen_diagnostics.json` or
`beamng_fsr_upscaling_diagnostics.json` into your BeamNG user folder with GPU,
API, OS, driver, settings, plan and the recent log. Attach it along with
`beamng_fsr\bridge.log` and `bridge.log.1`.
