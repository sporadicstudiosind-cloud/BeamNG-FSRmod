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

## 5. Three undocumented behaviours are assumed, not known

BeamNG documents neither its depth convention (reversed-Z vs standard, infinite
far plane), nor its velocity-buffer encoding (UV vs pixel space, sign, Y
direction), nor what Direct3D 12 resource state its PostFX system leaves a
render target in.

The third is the newest and the least forgiving. D3D12 offers **no way to query
a resource's current state** — it is tracked by the application, and this bridge
is not the application. FSR's backend transitions the resources it is handed
*from* the state the caller declares, so declaring the wrong one produces
incorrect barriers: at best a validation error, at worst a corrupted read with
no diagnostic at all.

There is no way to resolve this without a live install, so it is a setting
(*Advanced → Beacon resource state*, defaulting to pixel + compute read) with
the same treatment as the other two.

This project does **not** assert values it could not verify. Instead:

- Depth defaults to reversed-Z with an infinite far plane, with a UI override.
- Motion vectors default to UV space with Y flipped, with UI scale controls.
- The beacon resource state defaults to pixel + compute read, with a UI override.
- AMD's debug view (`Advanced → FSR debug view`) renders the visualisation that
  lets a user confirm or correct both in about a minute.
- Both defaults are logged loudly at initialisation.

Until confirmed against a live install, these remain **UNVERIFIED** in the
compatibility matrix.

## 6. Beacon overhead

The mod's PostFX beacon writes **two** full-resolution `R32G32B32A32_FLOAT`
targets every frame (16 bytes per pixel each) to publish depth and motion
vectors in a form the bridge can identify. At 1440p that is roughly 118 MB of
writes per frame plus the source reads.

Two targets rather than one packed target is not a choice. FSR reads depth from
channel 0 of one resource and the motion vector from channels 0 and 1 of
another; a single packed texture handed to both inputs would be read as a
motion vector of `(depth, velocity.x)`. Halving this cost would mean a format
narrower than 32-bit float for the motion vectors, which BeamNG's documented
format list does not clearly offer, or a compute pass inside the bridge to
unpack one target into two — recorded in `FUTURE_WORK.md`.

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
the code compiles against AMD's real headers, the portable logic passes 153
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

## 11. Upscaling does not execute in this build

**Frame generation runs. Upscaling does not.** Everything about the upscaling
product is real except the one layer that would let it do work: the FSR context,
the dispatch, the version selection, the planner, the settings and the UI all
exist and are exercised, and nothing calls the dispatch.

The reason is structural, not an oversight. Upscaling means presenting more
pixels than the game rendered. A DXGI-level layer can only arrange that by
**substituting the swapchain's backbuffers**: creating the real swapchain at
display resolution, handing BeamNG smaller textures in place of its backbuffers,
reporting the smaller size from `GetDesc`/`GetDesc1` so the engine sizes its
viewports to match, and running FSR from the small texture into the real
backbuffer at Present.

Frame generation needs none of that — AMD's interpolation swapchain presents at
the resolution the game already chose — which is exactly why one product
executes here and the other does not.

The bridge does not paper over this:

- Both upscaling capabilities are marked unavailable at startup with that
  reason, so the planner refuses and no plan is ever active.
- The panel leads with a red banner saying so and pointing at the frame
  generation product.
- The status pill reads *inactive*, because "active" now requires a live plan
  rather than merely an installed product.

`FUTURE_WORK.md` describes the substitution layer. It was not attempted here
because it is the highest-risk component in the design — a wrong swapchain
description gives a black screen rather than a degraded image — and it could not
be tested at all in this environment.

## 12. The per-frame path has never executed a frame

The Present hook, the command ring, the beacon resolution and the prepare
dispatch all compile against AMD's real API and are wired together. **None of it
has run.** No frame has been prepared, no allocator reset, no fence signalled.

The specific things that would surface first on real hardware:

- the beacon resource state (§5), which is the most likely single cause of a
  wrong image or a validation error;
- whether BeamNG's PostFX system mounts `shaders/` from a mod archive at all
  (§10) — if not, the beacons never exist and the bridge reports the inputs as
  unavailable;
- whether the two beacon targets really are allocated one frame apart as the
  staged enable intends, which is what their identity depends on.

Each fails visibly rather than silently: no beacon means no dispatch, and no
dispatch means frame generation stays off with a reason in the panel.

## 13. Legal posture

Injecting any DLL into BeamNG's process is not something BeamNG has explicitly
blessed. This project's reading of the EULA — that its prohibition targets
reverse engineering and modification of the Software, neither of which occurs
here — is set out in full in `LEGAL.md`. It is an interpretation, not a
permission. The feature is opt-in and the reasoning is documented so you can
disagree with it.
