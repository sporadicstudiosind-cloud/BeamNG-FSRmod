// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
#include "test_framework.hpp"

#include <cstring>

namespace bfsr::test {

int runAll(const char* filter) {
    auto& cases = Registry::instance().cases;
    size_t passed = 0;
    size_t failed = 0;
    size_t skipped = 0;
    std::string currentSuite;

    for (const auto& tc : cases) {
        const std::string full = tc.suite + "." + tc.name;
        if (filter && *filter && full.find(filter) == std::string::npos) {
            ++skipped;
            continue;
        }
        if (tc.suite != currentSuite) {
            currentSuite = tc.suite;
            std::printf("\n[%s]\n", currentSuite.c_str());
        }
        try {
            tc.fn();
            std::printf("  ok    %s\n", tc.name.c_str());
            ++passed;
        } catch (const Failure& f) {
            std::printf("  FAIL  %s\n        %s\n", tc.name.c_str(), f.message.c_str());
            ++failed;
        } catch (const std::exception& e) {
            std::printf("  FAIL  %s\n        unexpected exception: %s\n",
                        tc.name.c_str(), e.what());
            ++failed;
        } catch (...) {
            std::printf("  FAIL  %s\n        unexpected non-standard exception\n",
                        tc.name.c_str());
            ++failed;
        }
    }

    std::printf("\n%zu passed, %zu failed", passed, failed);
    if (skipped) std::printf(", %zu skipped by filter", skipped);
    std::printf("\n");
    return failed == 0 ? 0 : 1;
}

} // namespace bfsr::test

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : "";
    return bfsr::test::runAll(filter);
}
