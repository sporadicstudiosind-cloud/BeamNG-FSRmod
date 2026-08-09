# Changelog

## v0.2.0

Two independently installable products for BeamNG.drive's Direct3D 12 renderer,
built against AMD FSR SDK 2.3.0 "Redstone".

### AMD FSR Frame Generation — complete end to end

A Present hook drives every frame: IPC poll, plan resolution, context lifecycle,
beacon resolution, prepare dispatch, interpolation configure. AMD's
frame-interpolation swapchain presents the result. AMD Radeon Anti-Lag 2 where
the driver provides it.

- `SwapChainProxy` — a pass-through `IDXGISwapChain4`. BeamNG exposes no
  render-loop hook to mods, so Present is the only per-frame event a DXGI-level
  layer can observe, and it is also the correct one: by the time the game
  presents, depth and velocity are final.
- `FrameCommandRing` — FSR records into the bridge's own allocators, list and
  fence rather than borrowing one of BeamNG's, whose reset points the game owns.
  Three slots, each fence-gated before reuse.
- The frame-generation gate suppresses interpolation across pauses, loads,
  menus, replays, photo mode, occlusion, resolution changes, camera cuts and
  frame-time hitches, then holds off for a settling window so history rebuilds
  from clean frames.
- Auto mode applies AMD's own documented minimum base frame rates — 60 fps for
  FG 3.1.6, 30 fps for FG 4.0.1 — rather than a heuristic.
- The frame-generation multiplier is *reported*, not offered. SDK 2.3.0 gives
  the application no way to request one; `numGeneratedFrames` arrives already
  decided on the dispatch callback.

### AMD FSR Upscaling — does not execute

The FSR context, dispatch, planner, version selection, settings and UI are all
present, and nothing dispatches them. Presenting more pixels than BeamNG
rendered requires substituting the swapchain's backbuffers, which this bridge
does not do. Frame generation needs none of that, which is why one product runs
and the other does not.

The bridge does not paper over this: both upscaling capabilities are marked
unavailable at startup with that reason, the planner refuses, the panel leads
with a banner, and the status pill reads *inactive*. See `LIMITATIONS.md` §11
for the full account and `FUTURE_WORK.md` §0 for the design that would close it.

### The beacon publishes two targets

FSR reads depth from channel 0 of one resource and the motion vector from
channels 0 and 1 of another, so a single packed texture handed to both inputs
would be read as a motion vector of `(depth, velocity.x)`. Depth and velocity
are separate PostFX targets, told apart by creation order — and the order is
established rather than assumed: the depth pass is enabled alone for one frame
and the motion pass on the next, because Torque allocates a PostEffect's target
lazily on first render.

IPC protocol version 3.

### Separability

Both products ship the same bridge binaries and each drops a one-line manifest
into `beamng_fsr/modules/`. The shared runtime starts a feature only when its
manifest is present, so installing one does not drag in the other and deleting a
manifest is that product's off switch. CI enforces that every shared file is
byte-identical between the two archives and that neither has a loose file at its
root.

### Not modified

No BeamNG file is modified, overwritten or replaced. `dxgi.dll` and `d3d12.dll`
are proxies that forward to the genuine system DLLs; Windows loads them first
because the executable's own directory precedes the system directory in the DLL
search order.

### Verification, and what is missing

**Nothing in this release has been tested on hardware.** No Windows machine, no
GPU and no BeamNG install were available while building it.

What *was* verified:

- the bridge cross-compiles against AMD's real FSR SDK headers — no API invented;
- the proxy export tables match the system DLLs (`objdump -p`);
- 153 unit tests pass without a GPU, covering the capability model, both
  planners, frame pacing, the IPC protocol, beacon matching and module gating;
- both products package end to end and can be co-installed safely.

What is not:

- no frame has ever been executed — no allocator reset, no fence signalled;
- three BeamNG behaviours are assumed with UI overrides rather than known: the
  depth convention, the motion-vector encoding, and the Direct3D 12 resource
  state its PostFX leaves a render target in;
- every per-GPU compatibility row is marked NOT TESTED, and no performance or
  image-quality figure is claimed.

Read `docs/LIMITATIONS.md` before installing.
