cmake_minimum_required(VERSION 3.20)
project(beamng_fsr_bridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BFSR_BUILD_TESTS "Build the portable unit tests" ON)
option(BFSR_BUILD_BRIDGE "Build the Windows D3D12 bridge DLL" ${WIN32})

# Path to a checkout of GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.
#
# The AMD headers are deliberately NOT vendored into this repository: AMD's
# docs/license.md grants redistribution "in binary form only", which conflicts
# with the per-file MIT notices on the public headers. docs/LEGAL.md explains
# the conflict and why fetching is the conservative resolution.
# Run tools/fetch_fsr_sdk.py to populate this.
set(FSR_SDK_DIR "" CACHE PATH "Path to the AMD FSR SDK checkout (see tools/fetch_fsr_sdk.py)")

# ---------------------------------------------------------------------------
# bfsr_core — everything that does not touch a platform graphics API.
#
# Split out on purpose: this is the half that carries the decision logic
# (capabilities, planning, settings, IPC protocol, statistics) and it builds and
# tests on Linux/macOS as well as Windows, so the logic is actually covered by
# CI rather than only compiled.
# ---------------------------------------------------------------------------
add_library(bfsr_core STATIC
    src/core/json.cpp
    src/core/logging.cpp
    src/core/settings.cpp
    src/core/result.cpp
    src/caps/capabilities.cpp
    src/pipeline/plan.cpp
    src/pipeline/frame_pacing.cpp
    src/ipc/ipc_protocol.cpp
    src/diag/statistics.cpp
    src/d3d12/beacon_signature.cpp
    src/modules/module_registry.cpp
)
target_include_directories(bfsr_core PUBLIC include)
set_target_properties(bfsr_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(bfsr_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(bfsr_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# bfsr_bridge — the DXGI proxy DLL. Windows only, needs the AMD SDK headers.
# ---------------------------------------------------------------------------
if(BFSR_BUILD_BRIDGE)
    if(NOT FSR_SDK_DIR OR NOT EXISTS "${FSR_SDK_DIR}/Kits/FidelityFX/api/include/ffx_api.h")
        message(FATAL_ERROR
            "FSR_SDK_DIR is not set or does not look like an AMD FSR SDK checkout.\n"
            "Run:  python3 tools/fetch_fsr_sdk.py --dest third_party/FidelityFX-SDK\n"
            "then: cmake -DFSR_SDK_DIR=<abs path to that checkout> ...")
    endif()

    set(FSR_SDK_INCLUDES
        "${FSR_SDK_DIR}/Kits/FidelityFX/api/include"
        "${FSR_SDK_DIR}/Kits/FidelityFX/api/include/dx12"
        "${FSR_SDK_DIR}/Kits/FidelityFX/upscalers/include"
        "${FSR_SDK_DIR}/Kits/FidelityFX/framegeneration/include"
        "${FSR_SDK_DIR}/Kits/FidelityFX/framegeneration/include/dx12"
        "${FSR_SDK_DIR}/Kits/OpenSource/amd/antilag2")

    # --- the core: everything except the two entry-point stubs ---------------
    add_library(bfsr_bridge SHARED
        src/pipeline/bridge_runtime.cpp
        src/d3d12/resource_registry.cpp
        src/d3d12/device_proxy.cpp
        src/d3d12/factory_proxy.cpp
        src/d3d12/swapchain_proxy.cpp
        src/d3d12/frame_command_ring.cpp
        src/caps/capability_detector_d3d12.cpp
        src/ffx/ffx_loader.cpp
        src/ffx/fsr_upscaler.cpp
        src/ffx/fsr_framegen.cpp
        src/ffx/antilag2.cpp
        src/ipc/file_ipc.cpp
        src/core/paths_win.cpp
    )
    target_link_libraries(bfsr_bridge PRIVATE bfsr_core d3d12 dxgi)
    target_include_directories(bfsr_bridge PRIVATE include ${FSR_SDK_INCLUDES})
    target_compile_definitions(bfsr_bridge PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX BFSR_CORE_EXPORTS
        BFSR_VERSION_STRING="${PROJECT_VERSION}")
    set_target_properties(bfsr_bridge PROPERTIES OUTPUT_NAME "beamng_fsr_bridge" PREFIX "")

    # See src/ffx/antilag2.cpp for why Anti-Lag 2 is MSVC-only.
    # (kept as a no-op here so the option is discoverable in one place)
    # straight to a function pointer. MSVC accepts that; GCC and Clang reject it
    # as ill-formed. The header is AMD's redistributable source and is not
    # modified here, so the single translation unit that includes it is compiled
    # with the relaxed rule instead.
    if(NOT MSVC)
        set_source_files_properties(src/ffx/antilag2.cpp
            PROPERTIES COMPILE_OPTIONS "-fpermissive")
    endif()

    # --- the two proxy stubs -------------------------------------------------
    #
    # Named exactly dxgi.dll and d3d12.dll: Windows resolves them from the
    # executable's directory before the system directory, which is how the
    # bridge loads without modifying a single BeamNG file.
    function(bfsr_add_proxy_stub target source deffile outname)
        add_library(${target} SHARED ${source})
        target_include_directories(${target} PRIVATE include)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        set_target_properties(${target} PROPERTIES OUTPUT_NAME ${outname} PREFIX "")
        if(MSVC)
            set_target_properties(${target} PROPERTIES
                LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/${deffile}\"")
        else()
            target_sources(${target} PRIVATE ${deffile})
        endif()
    endfunction()

    bfsr_add_proxy_stub(bfsr_stub_dxgi  src/proxy/dxgi_proxy.cpp  src/proxy/dxgi_proxy.def  "dxgi")
    bfsr_add_proxy_stub(bfsr_stub_d3d12 src/proxy/d3d12_proxy.cpp src/proxy/d3d12_proxy.def "d3d12")
endif()

# ---------------------------------------------------------------------------
if(BFSR_BUILD_TESTS)
    enable_testing()
    add_executable(bfsr_tests
        tests/test_main.cpp
        tests/test_json.cpp
        tests/test_settings.cpp
        tests/test_capabilities.cpp
        tests/test_plan.cpp
        tests/test_frame_pacing.cpp
        tests/test_ipc_protocol.cpp
        tests/test_statistics.cpp
        tests/test_beacon_signature.cpp
        tests/test_modules.cpp
    )
    target_link_libraries(bfsr_tests PRIVATE bfsr_core)
    target_include_directories(bfsr_tests PRIVATE tests include)
    add_test(NAME bfsr_tests COMMAND bfsr_tests)
endif()
