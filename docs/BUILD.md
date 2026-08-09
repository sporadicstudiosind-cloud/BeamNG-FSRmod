# Building

## What you need

| | |
| --- | --- |
| CMake | 3.20+ |
| Compiler | MSVC 2022 for shipping builds. GCC/Clang builds the portable half and its tests on any platform. mingw-w64 cross-compiles the Windows bridge for verification. |
| Python | 3.9+, for the SDK fetch and packaging scripts |
| AMD FSR SDK | Fetched, not vendored — see `LEGAL.md` §2 |

## 1. Fetch the AMD SDK

```bash
python3 tools/fetch_fsr_sdk.py --dest third_party/FidelityFX-SDK
```

This clones the SDK (LFS objects skipped — the build does not need the media)
and verifies that every header the bridge compiles against and every signed DLL
the package redistributes is present, printing each binary's SHA-256.

## 2. Build and test the portable half

This is the layer carrying the capability model, the Auto-mode planner, settings
persistence, the IPC protocol, frame pacing and resource matching. It has no
platform graphics dependency and builds anywhere.

```bash
cmake -S native -B native/build -DBFSR_BUILD_BRIDGE=OFF
cmake --build native/build
./native/build/bfsr_tests
```

Expect `153 passed, 0 failed`. Filter with `./native/build/bfsr_tests Plan`.

## 3. Build the Windows bridge

### MSVC (shipping)

```bat
cmake -S native -B native\build-win -G "Visual Studio 17 2022" -A x64 ^
      -DBFSR_BUILD_BRIDGE=ON ^
      -DFSR_SDK_DIR=%CD%\third_party\FidelityFX-SDK
cmake --build native\build-win --config Release
```

### mingw-w64 (cross-compile, for verification)

```bash
cmake -S native -B native/build-win -G Ninja \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
  -DBFSR_BUILD_BRIDGE=ON -DBFSR_BUILD_TESTS=OFF \
  -DFSR_SDK_DIR="$PWD/third_party/FidelityFX-SDK"
cmake --build native/build-win
```

Three DLLs come out:

| Output | Role |
| --- | --- |
| `dxgi.dll` | Proxy stub; wraps `IDXGIFactory7` so swapchain creation is visible |
| `d3d12.dll` | Proxy stub; wraps `ID3D12Device` so texture creation is visible |
| `beamng_fsr_bridge.dll` | The core: FSR contexts, capability detection, planning, IPC |

Both stubs are needed: the device comes from `d3d12.dll` and the swapchain from
`dxgi.dll`.

**Anti-Lag 2 is MSVC-only.** AMD's `ffx_antilag2_dx12.h` contains a
`static_cast` from `void*` to a function pointer that MSVC accepts and GCC/Clang
reject as ill-formed. The header is AMD's redistributable source and is not
patched here; non-MSVC builds compile a guarded stub that reports Anti-Lag 2 as
unavailable rather than faking it.

## 4. Verify the exports

The proxies must export exactly the names the system DLLs do, or BeamNG fails to
start:

```bash
x86_64-w64-mingw32-objdump -p native/build-win/dxgi.dll | grep -A20 'Name Pointer'
```

## 5. Package

```bash
python3 tools/package.py \
  --build-dir native/build-win \
  --sdk-dir third_party/FidelityFX-SDK
```

Produces both products:

```
dist/framegen/beamng_fsr_framegen.zip     dist/framegen/bridge/
dist/upscaling/beamng_fsr_upscaling.zip   dist/upscaling/bridge/
```

Each `bridge/` folder holds the same three bridge DLLs plus AMD's loader, that
product's AMD effect DLL, its licence, and `beamng_fsr/modules/<id>.module.json`
— the manifest that tells the shared runtime the product is installed.

Build one product only with `--product framegen` or `--product upscaling`.

Two invariants the packager upholds, and CI re-checks: every file the two
products share is byte-identical, and neither archive contains a loose file at
its root (BeamNG's packing rules forbid those, and a root-level file would let
one product shadow the other's).

## Layout

```
native/
  include/beamng_fsr/   public headers
  src/core/             JSON, logging, settings, paths
  src/caps/             capability model (portable) + D3D12 detection
  src/modules/          module registry — which products are installed
  src/ffx/              AMD FSR loader, upscaler, frame generation, Anti-Lag 2
  src/d3d12/            device/factory proxies, resource registry, beacon matching
  src/pipeline/         the two planners, frame pacing, bridge runtime
  src/ipc/              protocol (portable) + one file channel per product
  src/proxy/            the two DLL stubs and their .def export lists
  tests/                153 tests over everything portable
mods/
  framegen/             frame generation mod ZIP source
  upscaling/            upscaling mod ZIP source
  shared/               files copied identically into both ZIPs
packaging/manifests/    the module manifests that gate each product
tools/                  SDK fetch, packaging
docs/                   this documentation
```

The `src/` split follows one rule: anything that can be tested without a GPU
lives in `bfsr_core` and is tested. Everything else is a thin layer over a
platform API.

The `mods/` split follows a second: anything byte-identical between the two
products lives in `mods/shared/` and is copied into both at package time, so
there is exactly one copy to maintain and co-installation cannot produce a
conflict.
