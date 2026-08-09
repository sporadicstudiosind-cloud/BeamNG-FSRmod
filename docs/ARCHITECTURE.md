# Architecture

The shape of this project is dictated by two facts established in
`FEASIBILITY.md`: BeamNG's Lua sandbox cannot load native code, and AMD's FSR
ships only a DirectX 12 backend. Everything below follows from those.

## The two halves

```
┌──────────────────────── BeamNG.drive process ─────────────────────────┐
│                                                                        │
│  BeamNG engine  (opaque — never inspected, patched or depended upon)  │
│      │                                                                 │
│      │ renders scene → #prepass[Depth], #VelocityBuffer[RT0]          │
│      ▼                                                                 │
│  ┌──────────────── MOD HALF (normal BeamNG mod ZIP) ───────────────┐  │
│  │  lua/ge/extensions/fsr/bridge.lua   state, IPC, settings        │  │
│  │  lua/ge/extensions/fsr/beacon.lua   PostEffect lifecycle        │  │
│  │  shaders/fsr/fsrBeaconP.hlsl        packs depth + velocity ─────┼──┼─┐
│  │  ui/modules/apps/FsrSettings/       settings and diagnostics    │  │ │
│  └─────────────────────────────────────────────────────────────────┘  │ │
│      │ file IPC (JSON, ~10 Hz)                                        │ │
│      ▼                                                                 │ │
│  ┌──────────── NATIVE HALF (Microsoft APIs only) ──────────────────┐  │ │
│  │  dxgi.dll   → DxgiFactoryProxy    → sees swapchain creation     │  │ │
│  │  d3d12.dll  → D3D12DeviceProxy    → sees texture creation ◄─────┼──┼─┘
│  │                                                                  │  │
│  │  beamng_fsr_bridge.dll                                          │  │
│  │    BridgeRuntime      orchestration, failure policy             │  │
│  │    FfxLoader          amd_fidelityfx_loader_dx12.dll            │  │
│  │    FsrUpscaler        ffxDispatchDescUpscale                    │  │
│  │    FsrFrameGenerator  FG context + interpolation swapchain      │  │
│  │    AntiLag2           AMD driver extension                      │  │
│  │    ResourceRegistry   descriptions of live textures             │  │
│  │    resolvePlan()      what actually runs, and why               │  │
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

So the mod half **publishes** them instead. A `PostEffect` copies
`#prepass[Depth]` and `#VelocityBuffer[RT0]` into a render target this mod
created, then sends that target's exact dimensions and format over IPC. The
bridge then searches for a texture it has a description of, rather than
inferring one from an engine it knows nothing about.

The identifying signature is created by us, not derived from BeamNG — which is
what makes it survive BeamNG updates.

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
| `bfsr_core` — JSON, settings, capabilities, planner, pacing, IPC protocol, beacon matching | nothing platform-specific | **118 unit tests, run in CI** |
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

## Frame generation flow

```
game frame N
  ├─ mod half: beacon PostEffect writes depth+velocity
  ├─ bridge:   FrameGenerationGate decides allow/suppress/reset
  ├─ bridge:   ffxDispatchDescFrameGenerationPrepareV2  (depth, MVs, camera)
  ├─ bridge:   ffxConfigureDescFrameGeneration          (enable, frameID)
  └─ Present → AMD's interpolation swapchain
                 └─ calls back on its own thread
                      └─ ffxDispatch → interpolated frame, paced
```

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
