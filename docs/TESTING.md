# Test plan

Two parts: what is automated and runs now, and what needs hardware and does not.

**Nothing in the hardware section has been executed.** It is written so someone
with the equipment can run it, not as a record of results.

---

## Part 1 — Automated (runs in CI, no GPU)

```bash
cmake -S native -B native/build -DBFSR_BUILD_BRIDGE=OFF
cmake --build native/build && ./native/build/bfsr_tests
```

118 tests over every module that can be tested without a graphics device.

| Suite | Tests | Covers |
| --- | --- | --- |
| `Json` | 11 | Parsing, escapes, surrogate pairs, malformed input, depth limits, key order |
| `Settings` | 13 | Round-trip, clamping, safe mode, wrong types, corrupt files, atomic save |
| `Capabilities` | 15 | One per GPU class, plus API/OS/shader-model gates and reason strings |
| `Plan` | 22 | Auto-mode decisions, downgrades, version pinning, VRAM guard, determinism |
| `FramePacing` + `FrameTimeTracker` | 13 | Every suppression reason, settling, hitch detection, percentile lows |
| `Ipc` | 7 | Round-trip, 64-bit precision, truncated documents, protocol mismatch |
| `Benchmark` | 11 | Rendered/generated separation, unmeasured-vs-zero, CSV/JSON export |
| `Beacon` + `Heuristic` | 15 | Exact/ambiguous/no match, stale resources, format predicates |

Three specific properties are asserted because getting them wrong would make the
project dishonest rather than merely broken:

1. **No unavailable feature is ever selected.** `test_plan.cpp` sweeps settings
   against capability sets.
2. **Every unavailable feature carries a non-trivial explanation.**
   `EveryUnavailableFeatureExplainsItself` fails on an empty or terse reason.
3. **ML upscaling and ML frame generation have different requirements.**
   `Rdna3GetsMlUpscalingButNotMlFrameGen` fails if they are ever conflated.

## Part 2 — Compile and package verification

| Check | Command |
| --- | --- |
| Bridge compiles against AMD's real headers | `cmake --build native/build-win` |
| Proxy exports match the system DLLs | `objdump -p native/build-win/dxgi.dll` |
| Lua parses | `luajit -bl mod/lua/ge/extensions/fsr/*.lua` |
| UI JavaScript parses | `node --check mod/ui/modules/apps/FsrSettings/app.js` |
| Packaging runs end to end | `python3 tools/package.py --sdk-dir ...` |

---

## Part 3 — Hardware testing (NOT EXECUTED)

### 3.1 Hardware to cover

| GPU | Why it matters |
| --- | --- |
| AMD RX 9000 (RDNA 4) | The only class where every feature including ML frame generation should be offered |
| AMD RX 7000 (RDNA 3) | Confirms ML upscaling is offered but ML frame generation is not |
| AMD RX 6000 (RDNA 2) | Confirms analytical-only, and that Anti-Lag 2 still works |
| NVIDIA RTX | Confirms analytical FSR works and no AMD-only option appears |
| Intel Arc | Same, on a third vendor |

For each: does the UI offer exactly what `COMPATIBILITY_MATRIX.md` predicts? A
mismatch either way is a bug — an option that should not be there, or a missing
one.

### 3.2 First-run checks

1. Bridge loads; `bridge.log` contains the environment block.
2. Diagnostics shows the correct GPU, `Direct3D 12`, and a plausible shader model.
3. Version dropdowns are populated from AMD's query, not empty.
4. Depth and motion-vector sources report `beacon (exact)`.
5. Launch under Vulkan → every option disabled with the Vulkan explanation.
6. Launch under D3D11 → same, with the D3D11 explanation.

### 3.3 Calibration (do this before anything else)

The depth and motion-vector defaults are unverified assumptions
(`LIMITATIONS.md` §5). Confirm them first or every later result is meaningless.

1. Enable `Advanced → FSR debug view`.
2. Drive forward at speed. Motion vectors should point opposite the motion,
   consistently, with no inversion in either axis.
3. If wrong, toggle `Flip motion vector Y`, then `Motion vectors are UV space`,
   then adjust the scales.
4. Record what worked. That turns an assumption into a fact.

### 3.4 Scenarios

Each at 1080p, 1440p and 4K, with FG off / FG on / upscaling on / both:

| Scenario | Watching for |
| --- | --- |
| Single vehicle, empty map | Baseline |
| Heavy traffic | Object motion vectors on many independent movers |
| High-speed driving | Large per-pixel motion; disocclusion |
| Night with headlights | Bright thin sources; ghosting behind lights |
| Rain | Particle motion without motion vectors |
| Smoke and dust | Transparency and the missing reactive mask |
| Deformation / crashes | Geometry changing shape between frames |
| Large map, streaming | Frame-time hitches → gate suppression |
| UI-heavy menus | UI interpolation and compositing |

### 3.5 State transitions

Each must not crash, not leak, and not produce a visible artefact on resume:

spawn vehicle · change vehicle · change map · reload level · pause/unpause ·
alt-tab · minimise/restore · resolution change · fullscreen↔windowed ·
switch FSR version at runtime · toggle FG while driving · enter/leave replay ·
enter/leave photo mode · disconnect a display · trigger a GPU device reset

### 3.6 Image-quality comparison

Capture the same frame under each configuration and compare:

native · FSR 3.1.x · FSR 4.x · FSR + FG · native + FG

Look specifically for: ghosting behind vehicles, shimmering on foliage and
fences, vehicle edge quality, thin geometry (aerials, wires), particles,
headlights and brake lights, reflections, rain streaks, smoke, UI artefacts,
motion-vector errors (smearing in one direction), and frame-generation artefacts
around fast-moving edges.

Do not declare one version better without evidence. Attach the captures.

### 3.7 Performance

Enable `Advanced → Benchmark mode`. Exports land in
`Bin64\beamng_fsr\benchmarks\` as CSV and JSON.

Record per configuration: average rendered FPS, average displayed FPS, 1% low,
0.1% low, frame time, upscaler GPU cost, frame-generation GPU cost, beacon pass
cost, VRAM, and latency if you have a way to measure it.

Two rules for reporting:

- **Never quote displayed FPS as if it were rendered FPS.** The exporter keeps
  them separate; keep them separate in the write-up.
- **An unmeasured stage is not a free stage.** The CSV leaves unmeasured GPU
  timings blank rather than writing 0, precisely so a spreadsheet cannot average
  them in as zero.

### 3.8 Failure injection

| Injection | Expected |
| --- | --- |
| Delete `amd_fidelityfx_upscaler_dx12.dll` | Upscaling unavailable with a named reason; FG still works |
| Delete `amd_fidelityfx_loader_dx12.dll` | FSR disabled entirely; BeamNG runs normally |
| Corrupt `settings.json` | Defaults restored, diagnostic logged, no crash |
| Truncate `request.json` mid-write | Ignored; previous settings retained |
| Kill BeamNG mid-session | Next launch starts in safe mode |
| Disable the mod half, keep the bridge | Falls back to heuristics; reports lower confidence or stays off |
| Set Lighting Quality to Lowest | "motion vectors unavailable" with the Lighting Quality hint |
