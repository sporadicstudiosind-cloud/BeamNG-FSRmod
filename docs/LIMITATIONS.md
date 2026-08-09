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
| **Upscaling** | The bridge substitutes the swapchain's backbuffers with render-resolution textures and upscales into the real one at present time (§11). | BeamNG's UI is rendered at the lower resolution and upscaled with the scene. |

A true "render at 1440p, present at 4K with a native-resolution UI" split
requires engine cooperation that does not exist. What the bridge can do is make
the *whole frame*, UI included, follow the render resolution — which is what
substitution does.

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

## 11. Upscaling substitutes the swapchain's backbuffers

This is the most invasive thing the bridge does, and it is worth understanding
before enabling it.

Upscaling means presenting more pixels than the game rendered. A DXGI-level
layer has exactly one way to arrange that: leave the real swapchain at display
resolution, hand BeamNG textures of the bridge's own at render resolution in
place of its backbuffers, and report the smaller size from `GetDesc` so the
engine sizes its viewports, depth buffer and render targets to match. At Present,
FSR reads the substitute and writes the real backbuffer.

Frame generation needs none of this — AMD's interpolation swapchain presents at
the resolution the game already chose — which is why it is the safer product.

What follows from the mechanism:

- **BeamNG's UI is upscaled with the scene.** The UI is drawn into what BeamNG
  believes is its backbuffer, which is the render-resolution substitute. A true
  "render at 1440p, present at 4K with a native-resolution UI" split needs
  engine cooperation that does not exist.
- **The failure mode is a black screen, not a soft image.** Once the game is
  rendering into the bridge's textures, something must write the real backbuffer
  every single frame. That is why substitution is refused unless the fallback
  blit was created first: a plain bilinear stretch is always available, so a
  frame FSR could not produce is soft rather than absent.
- **It is off unless you turn upscaling on.** Substitution is refused in safe
  mode, refused at a 1.0× ratio (Native AA), and refused if the substitutes
  cannot be created. With it off, the swapchain proxy is a pure pass-through.
- **Resize handling is a documented guess.** On a resize the engine may pass
  back the render size it was told, or a new display size it read from the
  window. The bridge treats zeros as "match the window", a size equal to the
  render size as a no-change resize, and anything else as a real display size.
  Both conventions land correctly, but which one BeamNG uses has not been
  observed.

One thing here is *not* a guess, unusually for this project. Direct3D 12
requires an application to transition a backbuffer to `PRESENT` before
presenting it, and BeamNG believes the substitutes are backbuffers — so their
resource state at Present is known rather than assumed, and FSR is told exactly
that. Contrast §5, where the beacons' state has to be a setting.

FSR also does not write the backbuffer directly. It declares its output as an
unordered-access resource, and a swapchain's buffers only support that if the
swapchain was created with `DXGI_USAGE_UNORDERED_ACCESS` — which an engine that
only ever renders into them has no reason to request. So FSR writes a
display-resolution texture the bridge owns and the result is copied across,
which asks nothing of the swapchain. That texture's format drops any sRGB
qualifier, because typed UAVs do not support sRGB; the copy is still legal,
since a format and its sRGB variant share a typeless family.

## 12. The per-frame path has never executed a frame

The Present hook, the command ring, the beacon resolution and the prepare
dispatch all compile against AMD's real API and are wired together. **None of it
has run.** No frame has been prepared, no allocator reset, no fence signalled.

The same is true of backbuffer substitution: no substitute texture has ever
been handed to a game, no `GetDesc` has ever returned a reduced size.

The specific things that would surface first on real hardware:

- the beacon resource state (§5), which is the most likely single cause of a
  wrong image or a validation error;
- whether BeamNG's PostFX system mounts `shaders/` from a mod archive at all
  (§10) — if not, the beacons never exist and the bridge reports the inputs as
  unavailable;
- whether the two beacon targets really are allocated one frame apart as the
  staged enable intends, which is what their identity depends on;
- whether BeamNG tolerates a swapchain that reports a smaller size than the
  window it is presenting into (§11), which is the assumption backbuffer
  substitution rests on.

Each fails visibly rather than silently: no beacon means no dispatch, and no
dispatch means frame generation stays off with a reason in the panel.

## 13. Legal posture

Injecting any DLL into BeamNG's process is not something BeamNG has explicitly
blessed. This project's reading of the EULA — that its prohibition targets
reverse engineering and modification of the Software, neither of which occurs
here — is set out in full in `LEGAL.md`. It is an interpretation, not a
permission. The feature is opt-in and the reasoning is documented so you can
disagree with it.
