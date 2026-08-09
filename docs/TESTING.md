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

146 tests over every module that can be tested without a graphics device.

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
| `Modules` | 11 | Manifest gating: neither installed, one, both, mismatched body, malformed, rescan |
| `PlanIndependence` | 5 | Neither planner reads the other's settings or changes when the other is absent |

Three specific properties are asserted because getting them wrong would make the
project dishonest rather than merely broken:

1. **No unavailable feature is ever selected.** `test_plan.cpp` sweeps settings
   against capability sets.
2. **Every unavailable feature carries a non-trivial explanation.**
   `EveryUnavailableFeatureExplainsItself` fails on an empty or terse reason.
3. **ML upscaling and ML frame generation have different requirements.**
   `Rdna3GetsMlUpscalingButNotMlFrameGen` fails if they are ever conflated.
4. **The two products are independent.** `PlanIndependence` fails if either
   planner's output changes because of the other, and `Modules` fails if one
   product's manifest can enable the other.
5. **AMD's own base-framerate thresholds are used, not an invented one.**
   `AutoUsesAmdsDocumentedThresholdPerImplementation` fails if a single
   threshold is applied to both implementations.

## Part 2 — Compile and package verification

| Check | Command |
| --- | --- |
| Bridge compiles against AMD's real headers | `cmake --build native/build-win` |
| Proxy exports match the system DLLs | `objdump -p native/build-win/dxgi.dll` |
| Lua parses | `find mods -name '*.lua' -exec luajit -bl {} \\;` |
| UI JavaScript parses | `find mods -name '*.js' -exec node --check {} \\;` |
| Packaging runs end to end | `python3 tools/package.py --sdk-dir ...` |
| The products can co-install | CI: overlapping files byte-identical, no loose root files |

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

### 3.2 Product-independence checks (do these first)

The whole premise of shipping two products is that either works alone. Verify
it before anything else:

| Installed | Expected |
| --- | --- |
| Frame generation only | Its panel works. No upscaling settings file appears. The log lists upscaling as "not installed". |
| Upscaling only | Its panel works. The swapchain is never wrapped — confirm no frame-generation lines in the log. |
| Both | Both panels work; each reports the other as installed. Shared bridge files are not duplicated or in conflict. |
| Both, then one manifest deleted | That product goes inert on the next launch; the other is unaffected. |

### 3.3 First-run checks

1. Bridge loads; `bridge.log` contains the environment block.
2. Diagnostics shows the correct GPU, `Direct3D 12`, and a plausible shader model.
3. Version dropdowns are populated from AMD's query, not empty.
4. Depth and motion-vector sources report `beacon (exact)`.
5. Launch under Vulkan → every option disabled with the Vulkan explanation.
6. Launch under D3D11 → same, with the D3D11 explanation.

### 3.4 Calibration (do this before measuring anything)

The depth and motion-vector defaults are unverified assumptions
(`LIMITATIONS.md` §5). Confirm them first or every later result is meaningless.

1. Enable `Advanced → FSR debug view`.
2. Drive forward at speed. Motion vectors should point opposite the motion,
   consistently, with no inversion in either axis.
3. If wrong, toggle `Flip motion vector Y`, then `Motion vectors are UV space`,
   then adjust the scales.
4. Record what worked. That turns an assumption into a fact.

### 3.5 Scenarios

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

### 3.6 State transitions

Each must not crash, not leak, and not produce a visible artefact on resume:

spawn vehicle · change vehicle · change map · reload level · pause/unpause ·
alt-tab · minimise/restore · resolution change · fullscreen↔windowed ·
switch FSR version at runtime · toggle FG while driving · enter/leave replay ·
enter/leave photo mode · disconnect a display · trigger a GPU device reset

### 3.7 Image-quality comparison

Capture the same frame under each configuration and compare:

native · FSR 3.1.x · FSR 4.x · FSR + FG · native + FG

Look specifically for: ghosting behind vehicles, shimmering on foliage and
fences, vehicle edge quality, thin geometry (aerials, wires), particles,
headlights and brake lights, reflections, rain streaks, smoke, UI artefacts,
motion-vector errors (smearing in one direction), and frame-generation artefacts
around fast-moving edges.

Do not declare one version better without evidence. Attach the captures.

### 3.8 Performance

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

### 3.9 Failure injection

| Injection | Expected |
| --- | --- |
| Delete `amd_fidelityfx_upscaler_dx12.dll` | Upscaling unavailable with a named reason; FG still works |
| Delete `amd_fidelityfx_loader_dx12.dll` | FSR disabled entirely; BeamNG runs normally |
| Corrupt `settings.json` | Defaults restored, diagnostic logged, no crash |
| Truncate `request.json` mid-write | Ignored; previous settings retained |
| Kill BeamNG mid-session | Next launch starts in safe mode |
| Disable the mod half, keep the bridge | Falls back to heuristics; reports lower confidence or stays off |
| Delete one product's module manifest | That product goes inert; the other is unaffected |
| Install both, then remove one product's ZIP | The remaining product keeps working; the removed panel disappears |
| Set Lighting Quality to Lowest | "motion vectors unavailable" with the Lighting Quality hint |
