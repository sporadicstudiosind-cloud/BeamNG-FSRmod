# Known limitations

Read this before installing. Several of these are structural — they cannot be
fixed by configuration, and some cannot be fixed by this project at all.

---

## 1. Camera jitter is not available — the headline limitation

FSR's temporal upscaler reconstructs sub-pixel detail by accumulating frames
that the *game* rendered with a jittered projection matrix. AMD exposes
`ffxQueryDescUpscaleGetJitterPhaseCount` / `GetJitterOffset` for exactly this.

**No documented BeamNG modding API can inject a per-frame jitter offset into
the scene projection matrix.** PostFX runs after rasterisation; Lua has no such
hook; the sandbox blocks native code (see `FEASIBILITY.md` §1.3).

What this means concretely:

- FSR upscaling still runs and still accumulates temporally, but it cannot
  recover sub-pixel information. Expect noticeably softer output than a native
  FSR integration in the same game at the same preset.
- This is **outside AMD's documented usage**. It is not a bug in this project
  and no setting fixes it.
- Only BeamNG can fix it, by integrating FSR natively or by exposing a
  projection-jitter hook to mods.
- The mod says so in the UI every time upscaling is enabled. It is not hidden.

**Frame generation is far less affected.** `jitterOffset` in the frame-generation
prepare dispatch describes the input frames' jitter, and zero is a coherent
value for an unjittered renderer. Frame generation is the stronger feature here
and is what this project recommends.

## 2. Render resolution is not independently controllable

The bridge cannot make BeamNG render its 3D scene at a resolution different from
the swapchain. Two workable modes result:

| Mode | How | Trade-off |
| --- | --- | --- |
| **Frame generation only** (recommended) | BeamNG renders at native; the bridge inserts interpolated frames. | Fully coherent. No image-quality cost to the rendered frames. |
| **Upscaling** | Lower BeamNG's own resolution; the bridge upscales to display resolution at present time. | BeamNG's UI is also rendered at the lower resolution and then upscaled. |

A true "render at 1440p, present at 4K with a native-resolution UI" split
requires engine cooperation that does not exist.

## 3. Vulkan and Direct3D 11 cannot be supported

Not "not yet" — cannot. AMD's FSR SDK 2.3.0 ships **DirectX 12 backends only**:
the readme lists Vulkan under known issues as unsupported, `signedbin/` contains
only `*_dx12.dll`, and every GPUOpen requirements page lists DirectX 12 alone.
FSR 3.1 and newer also require Direct3D 12 regardless.

The mod detects the active renderer and disables itself with an explanation.
Direct3D 12 has been BeamNG's default on Windows since v0.39, so most users are
already on the only viable path.

## 4. Device interface coverage

The D3D12 device wrapper implements the base `ID3D12Device` interface. If BeamNG
queries for `ID3D12Device1` or newer *and creates its render targets through
that interface*, those resources are not observed, the depth/motion-vector
beacon cannot be located, and FSR reports "render inputs unavailable" and stays
off.

This fails **safely and visibly** — never silently, never with a wrong image.
Extending the wrapper to the full `ID3D12Device1..14` chain is mechanical work,
listed in `FUTURE_WORK.md`.

The DXGI factory wrapper, by contrast, implements the complete
`IDXGIFactory..IDXGIFactory7` chain, because a partial wrapper there would lose
the swapchain silently rather than visibly.

## 5. Depth and motion-vector encodings are unverified assumptions

BeamNG documents neither its depth convention (reversed-Z vs standard, infinite
far plane) nor its velocity-buffer encoding (UV vs pixel space, sign, Y
direction).

This project does **not** assert values it could not verify. Instead:

- Depth defaults to reversed-Z with an infinite far plane, with a UI override.
- Motion vectors default to UV space with Y flipped, with UI scale controls.
- AMD's debug view (`Advanced → FSR debug view`) renders the visualisation that
  lets a user confirm or correct both in about a minute.
- Both defaults are logged loudly at initialisation.

Until confirmed against a live install, these remain **UNVERIFIED** in the
compatibility matrix.

## 6. Beacon overhead

The mod's PostFX beacon pass writes one full-resolution `R32G32B32A32_FLOAT`
target every frame (16 bytes per pixel) to publish depth and motion vectors in a
form the bridge can identify. At 1440p that is roughly 59 MB of writes per frame
plus the source reads.

This cost is real and has **not been measured on hardware** by this project.
`TESTING.md` includes it as a measurement to take.

## 7. BeamNG UI Hardware Acceleration

Community experience with ReShade shows BeamNG's CEF-based UI conflicts with
DXGI-level interception when *UI Hardware Acceleration* is enabled — the main
menu can become invisible. It also affects UI compositing during frame
generation, because with hardware acceleration on, the UI is not part of the
backbuffer in a form the frame-generation swapchain's UI-composition path can
cleanly consume.

If the menu disappears after installing, turn *UI Hardware Acceleration* off in
BeamNG's options.

## 8. Multiplayer

Third-party BeamNG multiplayer mods are out of scope and unsupported. A DXGI
proxy may violate their rules. The bridge does not attempt to conceal itself and
logs its presence on every start.

## 9. Nothing here has been tested on real hardware

**No performance figure, image-quality judgement, or hardware compatibility
result in this repository is claimed as measured.**

The build environment had no BeamNG installation, no AMD GPU and no Windows
machine. What *was* verified is stated precisely in the README's audit table:
the code compiles against AMD's real headers, the portable logic passes 118
unit tests, and the packaging pipeline runs end to end. Everything requiring
hardware is marked `NOT TESTED` in `COMPATIBILITY_MATRIX.md`, and `TESTING.md`
is written so someone with the hardware can execute it.

Fabricating those results would have been worse than not having them.

## 10. Mod archive layout is partly unverified

BeamNG's packing rules state a mod ZIP's root "must contain only top-level
folders" and list `vehicles`, `levels`, `art`, `assets`, `lua`, `scripts`, `ui`,
`gameplay`, `settings`, `trackEditor`, `vehicleGroups`. Both archives comply for
`lua/` and `ui/`, and neither contains a loose root file.

`shaders/` is **not** on that list. The beacon's HLSL lives there because that
mirrors the engine's own tree and is where a `ShaderData` path resolves from, and
because BeamNG's PostFX documentation references shaders by that kind of path.
It has not been verified that BeamNG mounts a `shaders/` folder from a mod
archive. If it does not, the beacon fails to create, the bridge reports the
render inputs as unavailable, and both products stay off — visibly, with a
reason, rather than silently.

## 11. Legal posture

Injecting any DLL into BeamNG's process is not something BeamNG has explicitly
blessed. This project's reading of the EULA — that its prohibition targets
reverse engineering and modification of the Software, neither of which occurs
here — is set out in full in `LEGAL.md`. It is an interpretation, not a
permission. The feature is opt-in and the reasoning is documented so you can
disagree with it.
