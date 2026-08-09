# Future extension architecture

Ordered by value, with the reasoning for each.

## 0. Backbuffer substitution — what makes upscaling execute

**Why:** it is the single missing piece for product 2. Everything else about
upscaling is built and unused: the FSR context, the dispatch, the version
selection, the planner, the settings and the UI. What does not exist is a way to
give the dispatch an input and an output (`LIMITATIONS.md` §11).

**Work:** intercept `CreateSwapChainForHwnd`, create the *real* swapchain at
display resolution, then hand BeamNG textures of the bridge's own at render
resolution in place of its backbuffers:

- `GetBuffer` returns the substitute textures.
- `GetDesc`/`GetDesc1` report the *reduced* size, so the engine sizes its
  viewports, depth buffer and render targets to match.
- Present runs the FSR upscale from the substitute into the real backbuffer,
  then forwards.
- `ResizeBuffers`/`ResizeBuffers1` and fullscreen transitions recreate both
  sides in step.

The resource states are the bridge's own throughout, which removes the
assumption §5 has to make for the beacons.

**Why it was not done first:** it is the highest-risk component in the whole
design. A wrong swapchain description gives a black screen rather than a
degraded image, the substitution has to stay coherent across fullscreen
transitions and resizes, and none of it could be tested in an environment with
no GPU, no Windows and no BeamNG. Frame generation reaches the same goal —
higher frame rate — through AMD's interpolation swapchain, which needs none of
this, so it was built first and completely.

It is also worth being clear about what upscaling would be worth once it runs:
BeamNG exposes no camera-jitter hook, so FSR's temporal upscaler would run
outside its designed operating conditions (`LIMITATIONS.md` §1). Frame
generation is barely affected by that. Upscaling is.

## 1. Extend the device wrapper to `ID3D12Device1..14`

**Why:** today only the base `ID3D12Device` is wrapped. If BeamNG creates render
targets through a newer interface, the bridge cannot observe them and FSR stops
with "render inputs unavailable" (`LIMITATIONS.md` §4).

**Work:** mechanical. Each version adds a handful of methods; the pattern is
already established in `device_proxy.cpp`, and the intercepted set stays the
same three creation calls plus their versioned variants
(`CreateCommittedResource1/2/3`).

**Why it was not done first:** it is ~70 forwarding methods of boilerplate, and
the current behaviour fails safely and visibly rather than silently. The factory
wrapper, where a gap *would* fail silently, is already complete.

## 1b. Unpack the beacons in the bridge instead of writing two targets

**Why:** the beacon costs two full-resolution `R32G32B32A32_FLOAT` writes per
frame (`LIMITATIONS.md` §6). One packed target plus a small compute pass in the
bridge that splits it into a depth texture and a motion-vector texture would
halve the PostFX bandwidth and remove the creation-order identity trick
entirely, since a single packed target is unambiguous on its own.

**Work:** a compute shader compiled at load time through `D3DCompile`, a root
signature, a PSO and a descriptor heap in the bridge, plus two owned output
textures.

**Why it was not done first:** it adds a shader pipeline to a component that
currently has none, and the two-target approach is correct without it. This is
an optimisation, not a fix.

## 2. Confirm the depth and motion-vector encodings

**Why:** these are the last unverified assumptions in the data path
(`LIMITATIONS.md` §5). Everything else has either a source or a test.

**Work:** ten minutes on a real install with the debug view (`TESTING.md` §3.3).
Once confirmed, the defaults become facts, the UI overrides become an advanced
escape hatch rather than a calibration requirement, and the compatibility matrix
loses its two `UNVERIFIED` cells.

## 3. GPU timestamp queries for the upscaler and frame-generation passes

**Why:** the diagnostics panel reports upscaler and frame-generation GPU cost
today, but only as fields the runtime can fill — nothing populates them yet, so
they correctly read "not measured". Real numbers would let Auto mode weigh cost
against benefit instead of using a fixed frame-rate threshold.

**Work:** an `ID3D12QueryHeap` of timestamp queries around each dispatch, with
a two-frame-deep readback ring so nothing stalls. `FrameSample` already carries
the fields and the CSV/JSON exporters already distinguish measured from
unmeasured.

## 4. A reactive mask

**Why:** FSR accepts `reactive` and `transparencyAndComposition` masks that tell
it which pixels are particles, transparencies and other things whose history
should not be trusted. Without them, BeamNG's rain, smoke and dust are the most
likely sources of ghosting.

**Work:** AMD provides `ffxDispatchDescUpscaleGenerateReactiveMask`, which needs
an opaque-only colour buffer and the composited one. Getting the opaque-only
buffer means a second beacon pass. Worth doing after §2 confirms the basics.

## 5. Exposure

**Why:** the bridge currently uses `FFX_UPSCALE_ENABLE_AUTO_EXPOSURE`, a
supported mode but not the best one. BeamNG exposes an `exposure` shader
constant to PostFX.

**Work:** a 1×1 render target written by a trivial beacon variant, passed as
`ffxDispatchDescUpscale::exposure`.

## 6. Camera basis for ML frame generation

**Why:** `ffxDispatchDescFrameGenerationPrepareV2` accepts camera position, up,
right and forward vectors. `FrameGenPrepareInputs` already carries them behind a
`cameraBasisValid` flag that is never set, so the prepare dispatch correctly
omits them rather than sending an identity basis.

**Work:** read the camera transform in Lua and send it over IPC. Cheap; the
plumbing exists.

## 7. FSR Ray Regeneration and Radiance Caching

**Why not yet:** both are in the SDK (`amd_fidelityfx_denoiser_dx12.dll`,
`amd_fidelityfx_radiancecache_dx12.dll`) but both are ray-tracing denoisers.
BeamNG's renderer does not produce the inputs they consume. This is not a
scheduling decision — there is currently nothing to feed them.

## 8. Vulkan

**Not a work item.** AMD ships no Vulkan FSR backend in SDK 2.3.0: the readme
lists Vulkan as unsupported and `signedbin/` contains only DX12 binaries. There
is nothing to integrate. If AMD ships one, the capability model already has a
`RendererApi::Vulkan` value and the gate that currently refuses it becomes the
place to allow it.

## 9. What would make this project unnecessary

Two things, both on BeamNG's side:

1. **A native plugin API** that lets a mod run compute work and receive the
   `ID3D12Device`. The entire proxy layer disappears.
2. **A projection-jitter hook.** This alone would fix the single largest quality
   limitation (`LIMITATIONS.md` §1) and turn FSR upscaling here from
   "compromised but working" into a real integration.

Either would be better than this architecture. This architecture exists because
neither is available.
