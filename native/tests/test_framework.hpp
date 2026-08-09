// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// A ~100-line test framework. The bridge deliberately has no third-party
// dependencies (it is loaded into another process before that process has
// finished initialising), and that discipline extends to the tests.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace bfsr::test {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

struct Registry {
    std::vector<TestCase> cases;
    static Registry& instance() {
        static Registry r;
        return r;
    }
};

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn) {
        Registry::instance().cases.push_back({suite, name, std::move(fn)});
    }
};

struct Failure {
    std::string message;
};

inline void fail(const char* file, int line, const std::string& what) {
    throw Failure{std::string(file) + ":" + std::to_string(line) + "  " + what};
}

inline bool nearlyEqual(double a, double b, double tol) {
    return std::fabs(a - b) <= tol;
}

int runAll(const char* filter);

} // namespace bfsr::test

#define BFSR_CONCAT_INNER(a, b) a##b
#define BFSR_CONCAT(a, b) BFSR_CONCAT_INNER(a, b)

#define TEST(suite, name)                                                       \
    static void BFSR_CONCAT(bfsr_test_fn_, __LINE__)();                         \
    static ::bfsr::test::Registrar BFSR_CONCAT(bfsr_test_reg_, __LINE__)(       \
        #suite, #name, &BFSR_CONCAT(bfsr_test_fn_, __LINE__));                  \
    static void BFSR_CONCAT(bfsr_test_fn_, __LINE__)()

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) ::bfsr::test::fail(__FILE__, __LINE__, "CHECK failed: " #cond); \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        const auto& bfsr_a = (a);                                               \
        const auto& bfsr_b = (b);                                               \
        if (!(bfsr_a == bfsr_b))                                                \
            ::bfsr::test::fail(__FILE__, __LINE__,                              \
                               "CHECK_EQ failed: " #a " == " #b);               \
    } while (0)

#define CHECK_NE(a, b)                                                          \
    do {                                                                        \
        if ((a) == (b))                                                         \
            ::bfsr::test::fail(__FILE__, __LINE__,                              \
                               "CHECK_NE failed: " #a " != " #b);               \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                   \
    do {                                                                        \
        if (!::bfsr::test::nearlyEqual((a), (b), (tol)))                        \
            ::bfsr::test::fail(__FILE__, __LINE__,                              \
                               "CHECK_NEAR failed: " #a " ~= " #b               \
                               " (got " + std::to_string((double)(a)) +         \
                               ", want " + std::to_string((double)(b)) + ")");  \
    } while (0)

#define CHECK_STR_EQ(a, b)                                                      \
    do {                                                                        \
        const std::string bfsr_a = (a);                                         \
        const std::string bfsr_b = (b);                                         \
        if (bfsr_a != bfsr_b)                                                   \
            ::bfsr::test::fail(__FILE__, __LINE__,                              \
                               "CHECK_STR_EQ failed: got \"" + bfsr_a +         \
                               "\", want \"" + bfsr_b + "\"");                  \
    } while (0)

/// Asserts a substring is present. Used heavily for the user-facing explanation
/// strings: those are a real deliverable, so they are tested like one.
#define CHECK_CONTAINS(haystack, needle)                                        \
    do {                                                                        \
        const std::string bfsr_h = (haystack);                                  \
        const std::string bfsr_n = (needle);                                    \
        if (bfsr_h.find(bfsr_n) == std::string::npos)                           \
            ::bfsr::test::fail(__FILE__, __LINE__,                              \
                               "CHECK_CONTAINS failed: \"" + bfsr_n +           \
                               "\" not found in \"" + bfsr_h + "\"");           \
    } while (0)
