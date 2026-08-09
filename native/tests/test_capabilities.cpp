// SPDX-License-Identifier: MIT
//
// These tests encode the published AMD requirements from docs/FEASIBILITY.md
// §4.2. If AMD changes a requirement, a test here should fail — that is the
// point: the capability table is the mod's promise not to offer options that
// cannot work.
#include "test_framework.hpp"

#include "beamng_fsr/capabilities.hpp"

using namespace bfsr;

namespace {

GpuInfo rdna4() {
    GpuInfo g;
    g.vendor = GpuVendor::AMD;
    g.architecture = GpuArchitecture::AmdRdna4;
    g.name = "AMD Radeon RX 9070 XT";
    g.vendorId = 0x1002;
    g.dedicatedVideoMemory = 16ull << 30;
    return g;
}

GpuInfo rdna3() {
    GpuInfo g = rdna4();
    g.architecture = GpuArchitecture::AmdRdna3;
    g.name = "AMD Radeon RX 7900 XTX";
    return g;
}

GpuInfo rdna2() {
    GpuInfo g = rdna4();
    g.architecture = GpuArchitecture::AmdRdna2;
    g.name = "AMD Radeon RX 6800 XT";
    return g;
}

GpuInfo nvidia() {
    GpuInfo g;
    g.vendor = GpuVendor::NVIDIA;
    g.architecture = GpuArchitecture::NvidiaGeneric;
    g.name = "NVIDIA GeForce RTX 4070";
    g.vendorId = 0x10DE;
    g.dedicatedVideoMemory = 12ull << 30;
    return g;
}

GpuInfo intelArc() {
    GpuInfo g;
    g.vendor = GpuVendor::Intel;
    g.architecture = GpuArchitecture::IntelArc;
    g.name = "Intel Arc B580";
    g.vendorId = 0x8086;
    g.dedicatedVideoMemory = 12ull << 30;
    return g;
}

SystemInfo win11Dx12Sm66() {
    SystemInfo s;
    s.isWindows = true;
    s.isWindows11OrGreater = true;
    s.windowsBuildNumber = 26100;
    s.highestShaderModel = 66;
    s.agilitySdkVersion = 1409;
    s.waveOpsSupported = true;
    s.renderer = RendererApi::D3D12;
    return s;
}

SystemInfo win10Dx12Sm66() {
    SystemInfo s = win11Dx12Sm66();
    s.isWindows11OrGreater = false;
    s.windowsBuildNumber = 19045;
    return s;
}

} // namespace

TEST(Capabilities, Rdna4Win11GetsEverything) {
    const auto caps = evaluateCapabilities(rdna4(), win11Dx12Sm66());
    CHECK(caps.isAvailable(Feature::UpscalingAnalytical));
    CHECK(caps.isAvailable(Feature::UpscalingMl));
    CHECK(caps.isAvailable(Feature::FrameGenerationAnalytical));
    CHECK(caps.isAvailable(Feature::FrameGenerationMl));
    CHECK(caps.isAvailable(Feature::AntiLag2));
}

TEST(Capabilities, Rdna3GetsMlUpscalingButNotMlFrameGen) {
    // FSR Upscaling 4.1.1 added RX 7000; ML frame generation is still RX 9000+.
    // Conflating the two is the specific mistake this test guards against.
    const auto caps = evaluateCapabilities(rdna3(), win11Dx12Sm66());
    CHECK(caps.isAvailable(Feature::UpscalingMl));
    CHECK(!caps.isAvailable(Feature::FrameGenerationMl));
    CHECK(caps.isAvailable(Feature::FrameGenerationAnalytical));
    CHECK_CONTAINS(caps.find(Feature::FrameGenerationMl)->unavailableReason, "RX 9000");
}

TEST(Capabilities, Rdna2GetsAnalyticalOnly) {
    const auto caps = evaluateCapabilities(rdna2(), win11Dx12Sm66());
    CHECK(caps.isAvailable(Feature::UpscalingAnalytical));
    CHECK(!caps.isAvailable(Feature::UpscalingMl));
    CHECK(caps.isAvailable(Feature::FrameGenerationAnalytical));
    CHECK(!caps.isAvailable(Feature::FrameGenerationMl));
    CHECK(caps.isAvailable(Feature::AntiLag2));
}

TEST(Capabilities, Windows10BlocksMlFrameGenOnly) {
    // AMD documents ML frame generation as Windows 11 only, while ML upscaling
    // is Win10/11. A user on Win10 + RX 9070 must keep ML upscaling.
    const auto caps = evaluateCapabilities(rdna4(), win10Dx12Sm66());
    CHECK(caps.isAvailable(Feature::UpscalingMl));
    CHECK(!caps.isAvailable(Feature::FrameGenerationMl));
    CHECK(caps.isAvailable(Feature::FrameGenerationAnalytical));
    CHECK_CONTAINS(caps.find(Feature::FrameGenerationMl)->unavailableReason, "Windows 11");
}

TEST(Capabilities, NvidiaGetsAnalyticalPathsButNoAmdMlOrAntiLag) {
    const auto caps = evaluateCapabilities(nvidia(), win11Dx12Sm66());
    CHECK(caps.isAvailable(Feature::UpscalingAnalytical));
    CHECK(caps.isAvailable(Feature::FrameGenerationAnalytical));
    CHECK(!caps.isAvailable(Feature::UpscalingMl));
    CHECK(!caps.isAvailable(Feature::FrameGenerationMl));
    CHECK(!caps.isAvailable(Feature::AntiLag2));
    CHECK_CONTAINS(caps.find(Feature::AntiLag2)->unavailableReason, "AMD");
}

TEST(Capabilities, IntelArcGetsAnalyticalPathsOnly) {
    const auto caps = evaluateCapabilities(intelArc(), win11Dx12Sm66());
    CHECK(caps.isAvailable(Feature::UpscalingAnalytical));
    CHECK(caps.isAvailable(Feature::FrameGenerationAnalytical));
    CHECK(!caps.isAvailable(Feature::UpscalingMl));
    CHECK(!caps.isAvailable(Feature::FrameGenerationMl));
}

TEST(Capabilities, VulkanDisablesEverythingWithAnExplanation) {
    SystemInfo s = win11Dx12Sm66();
    s.renderer = RendererApi::Vulkan;
    const auto caps = evaluateCapabilities(rdna4(), s);
    for (const auto& f : caps.features) {
        CHECK(!f.available);
        CHECK_CONTAINS(f.unavailableReason, "Vulkan");
        CHECK_CONTAINS(f.unavailableReason, "Direct3D 12");
    }
}

TEST(Capabilities, D3D11DisablesEverythingWithAnExplanation) {
    SystemInfo s = win11Dx12Sm66();
    s.renderer = RendererApi::D3D11;
    const auto caps = evaluateCapabilities(rdna4(), s);
    CHECK(!caps.anyUpscaling());
    CHECK(!caps.anyFrameGeneration());
    CHECK_CONTAINS(caps.find(Feature::UpscalingAnalytical)->unavailableReason, "Direct3D 11");
}

TEST(Capabilities, ShaderModel62BlocksMlPathsOnly) {
    SystemInfo s = win11Dx12Sm66();
    s.highestShaderModel = 62;
    const auto caps = evaluateCapabilities(rdna4(), s);
    CHECK(caps.isAvailable(Feature::UpscalingAnalytical));
    CHECK(caps.isAvailable(Feature::FrameGenerationAnalytical));
    CHECK(!caps.isAvailable(Feature::UpscalingMl));
    CHECK(!caps.isAvailable(Feature::FrameGenerationMl));
    CHECK_CONTAINS(caps.find(Feature::UpscalingMl)->unavailableReason, "6.6");
}

TEST(Capabilities, ShaderModel60BlocksEverything) {
    SystemInfo s = win11Dx12Sm66();
    s.highestShaderModel = 60;
    const auto caps = evaluateCapabilities(rdna4(), s);
    CHECK(!caps.anyUpscaling());
    CHECK(!caps.anyFrameGeneration());
    CHECK_CONTAINS(caps.find(Feature::UpscalingAnalytical)->unavailableReason, "6.2");
}

TEST(Capabilities, OldAgilitySdkBlocksMlFrameGen) {
    SystemInfo s = win11Dx12Sm66();
    s.agilitySdkVersion = 1400; // older than the documented 1.4.9 minimum
    const auto caps = evaluateCapabilities(rdna4(), s);
    CHECK(!caps.isAvailable(Feature::FrameGenerationMl));
    CHECK_CONTAINS(caps.find(Feature::FrameGenerationMl)->unavailableReason, "Agility SDK");
    // Upscaling has no Agility requirement and must be unaffected.
    CHECK(caps.isAvailable(Feature::UpscalingMl));
}

TEST(Capabilities, UnknownAgilitySdkDoesNotBlock) {
    SystemInfo s = win11Dx12Sm66();
    s.agilitySdkVersion = 0; // not determinable
    const auto caps = evaluateCapabilities(rdna4(), s);
    CHECK(caps.isAvailable(Feature::FrameGenerationMl));
}

TEST(Capabilities, SoftwareAdapterIsRefused) {
    GpuInfo g = rdna4();
    g.isSoftwareAdapter = true;
    const auto caps = evaluateCapabilities(g, win11Dx12Sm66());
    CHECK(!caps.isAvailable(Feature::UpscalingAnalytical));
    CHECK_CONTAINS(caps.find(Feature::UpscalingAnalytical)->unavailableReason, "software");
}

TEST(Capabilities, UnknownAmdArchitectureIsNotPreemptivelyRefused) {
    // A future AMD SKU this build predates must not be blocked by our advisory
    // architecture guess; AMD's runtime version query is the real gate.
    GpuInfo g = rdna4();
    g.architecture = GpuArchitecture::Unknown;
    g.name = "AMD Radeon RX 10800 XT";
    const auto caps = evaluateCapabilities(g, win11Dx12Sm66());
    CHECK(caps.isAvailable(Feature::UpscalingMl));
    CHECK(caps.isAvailable(Feature::FrameGenerationMl));
}

TEST(Capabilities, EveryUnavailableFeatureExplainsItself) {
    // The UI shows these strings verbatim, so an empty one is a real defect.
    const SystemInfo systems[] = {win11Dx12Sm66(), win10Dx12Sm66()};
    const GpuInfo gpus[] = {rdna4(), rdna3(), rdna2(), nvidia(), intelArc()};
    for (const auto& s : systems) {
        for (const auto& g : gpus) {
            for (const auto& f : evaluateCapabilities(g, s).features) {
                if (!f.available) {
                    CHECK(!f.unavailableReason.empty());
                    CHECK(f.unavailableReason.size() > 20);
                }
                CHECK(!f.requirement.empty());
            }
        }
    }
}

TEST(Capabilities, VendorIdMapping) {
    CHECK(vendorFromPciId(0x1002) == GpuVendor::AMD);
    CHECK(vendorFromPciId(0x10DE) == GpuVendor::NVIDIA);
    CHECK(vendorFromPciId(0x8086) == GpuVendor::Intel);
    CHECK(vendorFromPciId(0x1414) == GpuVendor::Microsoft);
    CHECK(vendorFromPciId(0xDEAD) == GpuVendor::Unknown);
}

TEST(Capabilities, ArchitectureGuessFromName) {
    CHECK(guessArchitectureFromName(GpuVendor::AMD, "AMD Radeon RX 9070 XT") == GpuArchitecture::AmdRdna4);
    CHECK(guessArchitectureFromName(GpuVendor::AMD, "AMD Radeon RX 7800 XT") == GpuArchitecture::AmdRdna3);
    CHECK(guessArchitectureFromName(GpuVendor::AMD, "AMD Radeon RX 6600") == GpuArchitecture::AmdRdna2);
    CHECK(guessArchitectureFromName(GpuVendor::AMD, "AMD Radeon RX 5700 XT") == GpuArchitecture::AmdRdna1);
    CHECK(guessArchitectureFromName(GpuVendor::AMD, "AMD Radeon RX 580") == GpuArchitecture::AmdGcnOrOlder);
    CHECK(guessArchitectureFromName(GpuVendor::Intel, "Intel(R) Arc(TM) B580") == GpuArchitecture::IntelArc);
    CHECK(guessArchitectureFromName(GpuVendor::NVIDIA, "NVIDIA GeForce RTX 5080") == GpuArchitecture::NvidiaGeneric);
    // No confident answer is better than a wrong one.
    CHECK(guessArchitectureFromName(GpuVendor::AMD, "Some Unlabelled Accelerator") == GpuArchitecture::Unknown);
}

TEST(Capabilities, EnvironmentDescriptionIsSerialisable) {
    const auto caps = evaluateCapabilities(rdna3(), win10Dx12Sm66());
    const Json j = describeEnvironment(rdna3(), win10Dx12Sm66(), caps);
    CHECK_STR_EQ(j["gpu"]["vendor"].asString(), "AMD");
    CHECK_STR_EQ(j["system"]["renderer"].asString(), "Direct3D 12");
    CHECK_EQ(j["features"].size(), size_t{5});
    // Round-trips through the parser, i.e. it is well-formed JSON.
    CHECK(!Json::parse(j.dump(2)).isNull());
}
