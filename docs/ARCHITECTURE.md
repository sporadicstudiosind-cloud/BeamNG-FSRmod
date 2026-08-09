# Architecture

The shape of this project is dictated by two facts established in
`FEASIBILITY.md`: BeamNG's Lua sandbox cannot load native code, and AMD's FSR
ships only a DirectX 12 backend. Everything below follows from those.

## Two products, one runtime

The frame generation and upscaling products are separately installable, and the
mechanism is deliberately boring: both ship the *same* bridge binaries, and each
drops a one-line manifest into `beamng_fsr/modules/`.

```
beamng_fsr/modules/framegen.module.json    → frame generation is installed
beamng_fsr/modules/upscaling.module.json   → upscaling is installed
```

`ModuleRegistry::scan` reads that directory at startup and the runtime
constructs a module's state only if its manifest is there. Concretely, with only
the frame generation manifest present: no `FsrUpscaler` is constructed, the
upscaling settings file is never opened, no upscaling status document is
written, and the upscaling IPC channel does not exist. The reverse holds, and
additionally the DXGI factory is not wrapped at all when frame generation is
absent — upscaling has no reason to touch presentation.

Two properties make co-installation safe, and both are checked in CI:

1. Every file the two products share is **byte-identical**, so copying one over
   the other is a no-op.
2. Neither mod archive contains a loose file at its root, which BeamNG's packing
   rules forbid and which would otherwise let one product's metadata shadow the
   other's.

The manifest is also the uninstall switch: deleting one disables that product
without touching the other.

## The two halves

```
┌──────────────────────── BeamNG.drive process ─────────────────────────┐
│                                                                        │
│  BeamNG engine  (opaque — never inspected, patched or depended upon)  │
│      │                                                                 │
│      │ renders scene → #prepass[Depth], #VelocityBuffer[RT0]          │
│      ▼                                                                 │
│  ┌──────────── MOD HALVES (one BeamNG mod ZIP per product) ────────┐  │
│  │  SHARED (byte-identical in both ZIPs)                           │  │
│  │    lua/ge/extensions/fsr_core/common.lua  state, IPC, beacon RC │  │
│  │    lua/ge/extensions/fsr_core/beacon.lua  PostEffect lifecycle   │  │
│  │    shaders/fsr/fsrDepthBeaconP.hlsl       depth ─────────────────┼──┼─┐
│  │    shaders/fsr/fsrMotionBeaconP.hlsl      velocity ──────────────┼──┼─┤
│  │  FRAME GENERATION           │  UPSCALING                        │  │ │
│  │    lua/.../fsr_fg/main.lua  │    lua/.../fsr_up/main.lua        │  │ │
│  │    ui/.../FsrFrameGeneration│    ui/.../FsrUpscaling            │  │ │
│  └─────────────────────────────────────────────────────────────────┘  │ │
│      │ one file IPC channel per product (JSON, ~10 Hz)                │ │
│      ▼                                                                 │ │
│  ┌──────────── NATIVE HALF (Microsoft APIs only) ──────────────────┐  │ │
│  │  dxgi.dll   → DxgiFactoryProxy    → sees swapchain creation     │  │ │
│  │  d3d12.dll  → D3D12DeviceProxy    → sees texture creation ◄─────┼──┼─┘
│  │  SwapChainProxy → Present, the per-frame tick                   │  │
│  │                                                                  │  │
│  │  beamng_fsr_bridge.dll   one runtime, two gated modules          │  │
│  │    ModuleRegistry     which products declared themselves        │  │
│  │    BridgeRuntime      module gating, orchestration, failsafe    │  │
│  │    FfxLoader          amd_fidelityfx_loader_dx12.dll            │  │
│  │    ── frame generation module ──────────────────────────────    │  │
│  │    FsrFrameGenerator  FG context + interpolation swapchain      │  │
│  │    AntiLag2           AMD driver extension                      │  │
│  │    resolveFrameGenPlan()                                        │  │
│  │    FrameCommandRing   FSR's own allocators/list/fence           │  │
│  │    ── upscaling module (built, NOT executed — LIMITATIONS §11) ─ │  │
│  │    FsrUpscaler        ffxDispatchDescUpscale — never dispatched  │  │
│  │    resolveUpscalePlan()                                         │  │
│  │    ── shared ───────────────────────────────────────────────    │  │
│  │    ResourceRegistry   descriptions of live textures             │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│      │ Present                                                         │
└────────┼───────────────────────────────────────────────────────────────┘
         ▼  display
```

## Why two proxy stubs

The D3D12 device is created by `d3d12.dll`'s `D3D12CreateDevice`; the swapchain
is created by a DXGI factory from `dxgi.dll`. Frame generation needs the
swapchain, and it needs depth and motion vectors as `ID3D12Resource` pointers,
which means seeing resource creation. One proxy cannot cover both.

Both stubs load the same `beamng_fsr_bridge.dll`. Windows refcounts modules, so
the second `LoadLibrary` returns the same instance — the two stubs share state
without any coordination protocol.

The stubs are deliberately tiny and paranoid. They run before anything else in
the process is ready, and if they misbehave BeamNG does not start. Every path
degrades to plain forwarding.

## The hard rule

> The native component hooks **only** Microsoft-provided APIs. No BeamNG symbol,
> offset, signature, vtable or binary is referenced, scanned, patched or
> depended upon.

This is a legal position (`LEGAL.md` §1) and an engineering one: there is
nothing version-specific to break, so BeamNG updates do not break the bridge.

It is why `D3D12DeviceProxy` and `DxgiFactoryProxy` are pass-through COM
wrappers and not vtable patches. A wrapper is inert — the game holds our object,
we hold the real one, and on any failure we hand the real pointer back and
disappear.

## The beacon

A D3D12-level layer sees hundreds of textures a frame and cannot tell which is
the depth buffer. Classifying by format and size is fragile and breaks whenever
BeamNG changes a render target.

So the mod half **publishes** them instead. Two `PostEffect` passes copy
`#prepass[Depth]` and `#VelocityBuffer[RT0]` into render targets this mod
created, then send those targets' exact dimensions and formats over IPC. The
bridge then searches for textures it has a description of, rather than
inferring them from an engine it knows nothing about.

The identifying signature is created by us, not derived from BeamNG — which is
what makes it survive BeamNG updates.

**Two targets, not one.** FSR reads depth from channel 0 of one resource and the
motion vector from channels 0 and 1 of another. A single packed RGBA target
handed to both inputs would be read as a motion vector of
`(depth, velocity.x)` — a plausible-looking image that is wrong. So depth and
velocity are separate targets.

That leaves them with identical descriptions, so they are told apart by creation
order. Order is *established* rather than assumed: Torque allocates a
PostEffect's target lazily on its first render, so the mod enables the depth
pass alone for one frame and the motion pass on the next. Neither signature is
published until both are up, and the matcher requires exactly the expected
number of candidates before it calls a match Exact. Getting this backwards would
feed depth in as motion vectors, which is why it is built to be deterministic
instead of probable.

`classifyHeuristically()` remains as a fallback for when the mod half is absent,
and reports lower confidence. If it is ambiguous, FSR stays off rather than
guessing.

## Version selection is AMD's, not ours

Nothing about which FSR implementations exist is hardcoded:

```
ffxQuery(FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, device)  → id/name list
ffxOverrideVersion{versionId}                            → pin one
```

The query takes the `ID3D12Device`, so AMD's driver has already filtered the
list against the actual hardware. That makes it a far better authority than any
device-ID table this project could maintain, and it means a newer AMD SDK
exposes newer implementations with no code change here.

If the list comes back empty, that overrides the capability table: AMD is saying
this device has no provider, and the UI says exactly that.

## Layering, and what is testable

| Layer | Depends on | Tested |
| --- | --- | --- |
| `bfsr_core` — JSON, settings, capabilities, both planners, pacing, IPC protocol, beacon matching, module registry | nothing platform-specific | **153 unit tests, run in CI** |
| `bfsr_bridge` — D3D12/DXGI proxies, FFX contexts, detection, runtime | D3D12, DXGI, AMD FSR | compile-verified against AMD's real headers |
| stubs | Windows loader | export tables verified against the system DLLs |

The split exists so the decision logic — the part where a mistake means offering
a user an option their hardware cannot run — is covered by tests that need no
GPU.

## Failure policy

Stated once and applied everywhere: **BeamNG must keep running.**

- Every exported C entry point catches everything. An exception must not cross
  the DLL boundary.
- Every wrap function returns the game's original object on any failure.
- A missing AMD runtime disables FSR; it does not prevent BeamNG from starting.
- Missing or ambiguous render inputs disable FSR rather than substituting data.
- A crash sentinel file written at startup and deleted on clean shutdown means
  the next launch starts in safe mode rather than repeating what killed it.

FSR turning itself off is always preferable to a crash or a wrong image.

## The per-frame driver

Everything else in the bridge is driven by an event that happens once: device
creation, factory creation, swapchain creation. FSR needs something that happens
every frame — the prepare dispatch has to be recorded after the scene is drawn
and before the frame is presented, and frame generation has to be reconfigured
with a new frame ID each time.

BeamNG exposes no render-loop hook to mods, so the only per-frame event a
DXGI-level layer can legitimately observe is **Present**. `SwapChainProxy` is a
pass-through `IDXGISwapChain4` that calls the runtime immediately before
forwarding it.

Present is also the correct point rather than merely the available one: by the
time the game presents, the depth and velocity buffers are final and the beacon
passes have already copied them.

Composition order is load-bearing:

```
game  →  SwapChainProxy  →  AMD frame-interpolation swapchain  →  DXGI
         (per-frame tick)    (schedules the generated frame)
```

Wrapping in the other order would prepare a frame that had already been
presented.

The whole `IDXGISwapChain..4` chain is implemented, for the same reason the DXGI
factory proxy implements its whole chain: a partial wrapper fails
`QueryInterface`, the game falls back to the unwrapped object, and Present stops
reaching the hook — silently, with frame generation simply never running.

FSR's own GPU work is recorded into the bridge's own command list, never one of
BeamNG's: the game owns its allocators' reset points, and recording into a list
it is about to reset is a use-after-free with GPU timing. `FrameCommandRing`
holds three allocators with a fence each, so a slot is only reused once the GPU
has signalled its previous submission.

## Frame generation flow

```
game frame N
  ├─ mod half: two beacon PostEffects write depth and velocity
  ├─ SwapChainProxy::Present  ── the tick ──┐
  │    ├─ bridge: poll IPC (paced, not every frame) → settings, game state
  │    ├─ bridge: resolve the beacon targets from the resource registry
  │    ├─ bridge: resolveFrameGenPlan()
  │    ├─ bridge: FrameGenerationGate decides allow/suppress/reset
  │    ├─ bridge: ring.begin() → ffxDispatch…PrepareV2 → ring.submit()
  │    └─ bridge: ffxConfigureDescFrameGeneration (enable, frameID)
  └─ forwarded Present → AMD's interpolation swapchain
                 └─ calls back on its own thread
                      └─ ffxDispatch → interpolated frame, paced
```

`configure` runs on every frame including suppressed ones: it is what tells
AMD's swapchain *not* to interpolate this one, and skipping it would leave the
previous frame's decision in force.

**Upscaling has no equivalent flow.** It has a real FSR context and a real
dispatch that nothing calls, because presenting more pixels than BeamNG rendered
needs a backbuffer-substitution layer this build does not have. The bridge marks
both upscaling capabilities unavailable at startup with that reason rather than
letting the panel imply otherwise. See `LIMITATIONS.md` §11.

The gate is what stops interpolation across pauses, loads, menus, replays,
photo mode, occlusion, resolution changes, camera cuts and frame-time hitches,
and holds off for a settling window afterwards so history rebuilds from clean
frames. It is pure logic and fully unit-tested.

## Extension points

`FUTURE_WORK.md` lists the concrete ones. The two that matter architecturally:

- **`IFrameInputProvider`** is effectively already there in shape: beacon and
  heuristic are two strategies behind one matching interface. A third —
  engine-native, if BeamNG ever exposes one — drops in without touching the
  runtime.
- **Backends** are selected entirely through AMD's version IDs, so a future FSR
  implementation needs no new backend class at all.
