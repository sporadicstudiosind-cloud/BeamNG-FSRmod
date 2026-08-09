# Compatibility matrix

**Every hardware row below is `NOT TESTED`.** The build environment had no
Windows machine, no BeamNG installation and no GPU of any vendor. Rows describe
what AMD's published requirements and this project's capability model *predict*,
not what anyone observed.

A row marked `NOT TESTED` is an invitation to test it, not a claim.

## How to read this

| Marker | Meaning |
| --- | --- |
| `predicted ✔` | AMD documents support and the capability model enables it. Unverified on hardware. |
| `predicted ✘` | AMD documents no support; the model hides the option and gives a reason. |
| `runtime-gated` | The final answer comes from AMD's `ffxQuery` version enumeration against your actual device and driver, which overrides this table. |

## GPUs

| GPU | Analytical upscaling (FSR 3.1.5) | ML upscaling (FSR 4.1.1) | Analytical frame gen (3.1.x) | ML frame gen (4.0.x) | Anti-Lag 2 | Tested |
| --- | --- | --- | --- | --- | --- | --- |
| AMD RX 9000 (RDNA 4) | predicted ✔ | predicted ✔ | predicted ✔ | predicted ✔ (Win 11 only) | predicted ✔ | **NOT TESTED** |
| AMD RX 7000 (RDNA 3) | predicted ✔ | predicted ✔ | predicted ✔ | predicted ✘ | predicted ✔ | **NOT TESTED** |
| AMD RX 6000 (RDNA 2) | predicted ✔ | predicted ✘ | predicted ✔ | predicted ✘ | predicted ✔ | **NOT TESTED** |
| AMD RX 5000 (RDNA 1) | predicted ✔ | predicted ✘ | predicted ✔ | predicted ✘ | predicted ✔ | **NOT TESTED** |
| AMD RX 500 / GCN | predicted ✔ (needs SM 6.2) | predicted ✘ | predicted ✔ | predicted ✘ | predicted ✔ | **NOT TESTED** |
| NVIDIA RTX 20/30/40/50 | predicted ✔ | predicted ✘ | predicted ✔ | predicted ✘ | predicted ✘ | **NOT TESTED** |
| NVIDIA GTX 10 series | predicted ✔ if SM 6.2 | predicted ✘ | predicted ✔ if SM 6.2 | predicted ✘ | predicted ✘ | **NOT TESTED** |
| Intel Arc A/B series | predicted ✔ | predicted ✘ | predicted ✔ | predicted ✘ | predicted ✘ | **NOT TESTED** |
| Intel integrated | predicted ✔ if SM 6.2 | predicted ✘ | predicted ✔ if SM 6.2 | predicted ✘ | predicted ✘ | **NOT TESTED** |

Sources for the ✔/✘ pattern: [FSR Upscaling](https://gpuopen.com/amd-fsr-upscaling/),
[FSR Frame Generation](https://gpuopen.com/amd-fsr-framegeneration/). AMD lists
analytical FSR for "Radeon RX 500 Series GPUs and above (or equivalent from
other GPU vendors)" at SM 6.2; ML upscaling for "RX 9000 and 7000 Series
discrete GPUs" at SM 6.6; ML frame generation for "RX 9000 Series and above" on
Windows 11 only.

Note the asymmetry that is easy to get wrong: **ML upscaling covers RDNA 3, ML
frame generation does not.** They are modelled as separate capability bits and
`test_capabilities.cpp` asserts the distinction.

## Graphics APIs

| API | Supported | Why |
| --- | --- | --- |
| Direct3D 12 | **Yes** | The only backend AMD ships. BeamNG's default renderer since v0.39. |
| Vulkan | **No, permanently** | AMD's SDK 2.3.0 readme lists Vulkan as unsupported and `signedbin/` contains no Vulkan loader. Not a future work item — there is nothing to integrate. |
| Direct3D 11 | **No** | FSR 3.1+ requires D3D12. BeamNG itself calls D3D11 an obsolete fallback. |

## Operating systems

| OS | Analytical FSR | ML upscaling | ML frame generation |
| --- | --- | --- | --- |
| Windows 11 | predicted ✔ | predicted ✔ | predicted ✔ |
| Windows 10 | predicted ✔ | predicted ✔ | **predicted ✘** — AMD documents ML frame generation as Windows 11 only |
| Linux (native / Proton) | ✘ | ✘ | ✘ | 

Native Linux BeamNG uses Vulkan, so none of this applies. Under Proton the
bridge is untested and the DXGI proxy mechanism behaves differently; treat it as
unsupported.

## BeamNG versions

| Version | Status |
| --- | --- |
| 0.39.x | **Target.** D3D12 is the default renderer. Untested on hardware. |
| 0.38 and older | Unknown. D3D12 was not the default; the bridge should detect and disable, but this is untested. |
| Future versions | The bridge hooks only Microsoft APIs and references no BeamNG symbol, so a BeamNG update should not break it. The mod half depends on documented PostFX target names (`#prepass[Depth]`, `#VelocityBuffer[RT0]`), which could change. |

## Resolutions

| Resolution | Status |
| --- | --- |
| 1920×1080 | **NOT TESTED** |
| 2560×1440 | **NOT TESTED** |
| 3840×2160 | **NOT TESTED** |

## What *was* verified

To be precise about the difference between "tested" and "unverified":

| Item | Verified how |
| --- | --- |
| FSR API usage is real and current | Compiled against AMD FSR SDK 2.3.0's actual headers with mingw-w64 |
| The bridge links into three correctly-exported DLLs | `objdump -p` export tables match the system DLLs |
| Capability rules match AMD's published requirements | 118 unit tests, including one per GPU class above |
| Planner never selects an unavailable feature | Unit tests over the full settings × capability matrix |
| Settings survive save/load/corruption | Unit tests including malformed and truncated input |
| Packaging produces both artefacts | `tools/package.py` executed end to end |
