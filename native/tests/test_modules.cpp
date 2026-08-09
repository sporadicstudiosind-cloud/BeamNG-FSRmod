// SPDX-License-Identifier: MIT
//
// The module registry is what makes the two products independently
// installable, so its behaviour is pinned here: present means enabled, absent
// means completely inert, and one product's manifest must never enable the
// other.
#include "test_framework.hpp"

#include "beamng_fsr/module_registry.hpp"

#include <cstdio>
#include <fstream>
#include <string>

using namespace bfsr;

namespace {

const std::string kDir = "bfsr_test_modules/";

void makeDir() {
#if defined(_WIN32)
    std::system(("mkdir " + kDir).c_str());
#else
    std::system(("mkdir -p " + kDir).c_str());
#endif
}

void writeManifest(const std::string& name, const std::string& contents) {
    makeDir();
    std::ofstream out(kDir + name, std::ios::trunc);
    out << contents;
}

void cleanup() {
    std::remove((kDir + "framegen.module.json").c_str());
    std::remove((kDir + "upscaling.module.json").c_str());
}

std::string manifestFor(const char* module, const char* version = "1.0.0") {
    return std::string("{\"module\":\"") + module + "\",\"version\":\"" + version + "\"}";
}

} // namespace

TEST(Modules, NothingInstalledIsInert) {
    cleanup();
    ModuleRegistry registry;
    registry.scan(kDir);
    CHECK(!registry.anyPresent());
    CHECK(!registry.isPresent(ModuleId::FrameGeneration));
    CHECK(!registry.isPresent(ModuleId::Upscaling));
}

TEST(Modules, MissingDirectoryIsNotAnError) {
    ModuleRegistry registry;
    registry.scan("definitely_not_a_directory_bfsr/");
    CHECK(!registry.anyPresent());
    registry.scan("");
    CHECK(!registry.anyPresent());
}

TEST(Modules, FrameGenAloneEnablesOnlyFrameGen) {
    cleanup();
    writeManifest("framegen.module.json", manifestFor("framegen", "0.2.0"));
    ModuleRegistry registry;
    registry.scan(kDir);
    CHECK(registry.isPresent(ModuleId::FrameGeneration));
    CHECK(!registry.isPresent(ModuleId::Upscaling));
    CHECK_STR_EQ(registry.find(ModuleId::FrameGeneration)->version, "0.2.0");
    cleanup();
}

TEST(Modules, UpscalingAloneEnablesOnlyUpscaling) {
    cleanup();
    writeManifest("upscaling.module.json", manifestFor("upscaling"));
    ModuleRegistry registry;
    registry.scan(kDir);
    CHECK(registry.isPresent(ModuleId::Upscaling));
    CHECK(!registry.isPresent(ModuleId::FrameGeneration));
    cleanup();
}

TEST(Modules, BothInstalledCoexist) {
    cleanup();
    writeManifest("framegen.module.json", manifestFor("framegen"));
    writeManifest("upscaling.module.json", manifestFor("upscaling"));
    ModuleRegistry registry;
    registry.scan(kDir);
    CHECK(registry.isPresent(ModuleId::FrameGeneration));
    CHECK(registry.isPresent(ModuleId::Upscaling));
    CHECK_EQ(registry.modules().size(), size_t{2});
    cleanup();
}

TEST(Modules, MismatchedBodyCannotEnableTheOtherProduct) {
    // A framegen.module.json claiming to be the upscaling module must not
    // enable upscaling — otherwise one product could silently turn on the
    // other, which defeats the whole point of the split.
    cleanup();
    writeManifest("framegen.module.json", manifestFor("upscaling"));
    ModuleRegistry registry;
    registry.scan(kDir);
    CHECK(!registry.isPresent(ModuleId::Upscaling));
    CHECK(!registry.isPresent(ModuleId::FrameGeneration));
    cleanup();
}

TEST(Modules, MalformedManifestIsIgnored) {
    cleanup();
    writeManifest("framegen.module.json", "{ this is not json");
    ModuleRegistry registry;
    registry.scan(kDir);
    CHECK(!registry.anyPresent());

    writeManifest("framegen.module.json", "{\"module\":\"something_else\"}");
    registry.scan(kDir);
    CHECK(!registry.anyPresent());
    cleanup();
}

TEST(Modules, RescanReplacesPreviousState) {
    cleanup();
    writeManifest("framegen.module.json", manifestFor("framegen"));
    ModuleRegistry registry;
    registry.scan(kDir);
    CHECK(registry.isPresent(ModuleId::FrameGeneration));

    // Uninstalling the product removes its manifest; a rescan must forget it.
    cleanup();
    registry.scan(kDir);
    CHECK(!registry.isPresent(ModuleId::FrameGeneration));
}

TEST(Modules, ForcePresentIsIdempotent) {
    ModuleRegistry registry;
    registry.forcePresent(ModuleId::Upscaling, "test");
    registry.forcePresent(ModuleId::Upscaling, "test-again");
    CHECK_EQ(registry.modules().size(), size_t{1});
    CHECK_STR_EQ(registry.find(ModuleId::Upscaling)->version, "test");
}

TEST(Modules, DescriptionCoversBothProductsEitherWay) {
    ModuleRegistry registry;
    registry.forcePresent(ModuleId::FrameGeneration, "1.0");
    const auto lines = registry.describe();
    CHECK_EQ(lines.size(), size_t{2});
    CHECK_CONTAINS(lines[0], "installed");
    CHECK_CONTAINS(lines[1], "not installed");
}

TEST(Modules, SerialisesBothSlotsAlways) {
    // The UI needs to know a product is *absent*, not just omitted, so both
    // slots appear in the JSON regardless.
    ModuleRegistry registry;
    registry.forcePresent(ModuleId::FrameGeneration, "1.0");
    const Json j = registry.toJson();
    CHECK_EQ(j.size(), size_t{2});
    CHECK(j.at(0)["installed"].asBool());
    CHECK(!j.at(1)["installed"].asBool());
}

TEST(Modules, ManifestFileNames) {
    CHECK_STR_EQ(moduleManifestFileName(ModuleId::FrameGeneration), "framegen.module.json");
    CHECK_STR_EQ(moduleManifestFileName(ModuleId::Upscaling), "upscaling.module.json");
}
