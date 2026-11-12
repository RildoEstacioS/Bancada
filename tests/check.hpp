// A very small test runner, so the simulator's tests need nothing but a compiler.
#pragma once

#include <cstdio>
#include <vector>

namespace check {

struct Test {
    const char* name;
    void (*run)();
};

inline std::vector<Test>& tests() {
    static std::vector<Test> all;
    return all;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, void (*run)()) { tests().push_back({name, run}); }
};

inline int runAll() {
    int failedTests = 0;
    for (const Test& test : tests()) {
        const int before = failures();
        test.run();
        const bool ok = failures() == before;
        std::printf("%s  %s\n", ok ? "ok  " : "FAIL", test.name);
        if (!ok) ++failedTests;
    }
    std::printf("\n%zu tests, %d failed\n", tests().size(), failedTests);
    return failedTests == 0 ? 0 : 1;
}

}  // namespace check

#define TEST(name)                                         \
    static void name();                                    \
    static const check::Registrar name##_reg{#name, name}; \
    static void name()

#define CHECK(...)                                                              \
    do {                                                                        \
        if (!(__VA_ARGS__)) {                                                   \
            ++check::failures();                                                \
            std::printf("  %s:%d: failed: %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
        }                                                                       \
    } while (false)

#define CHECK_EQ(actual, expected)                                                     \
    do {                                                                               \
        const unsigned long long a_ = static_cast<unsigned long long>(actual);         \
        const unsigned long long e_ = static_cast<unsigned long long>(expected);       \
        if (a_ != e_) {                                                                \
            ++check::failures();                                                       \
            std::printf("  %s:%d: %s is %llu, expected %llu\n", __FILE__, __LINE__, #actual, a_, e_); \
        }                                                                              \
    } while (false)
