#pragma once
// Zero-dependency test framework. Each test binary registers AX_TEST cases and
// defines AX_TEST_MAIN as its entry point. A single optional argv[1] filters
// test names by substring. CHECK failures mark the test failed but continue
// running it to surface all problems in one pass.

#include <cstdio>
#include <cstring>
#include <vector>

namespace testfw {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
inline bool& current_failed() {
    static bool f = false;
    return f;
}

struct Registrar {
    Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

inline int run_all(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    size_t failed = 0, ran = 0;
    for (const auto& tc : registry()) {
        if (filter && !std::strstr(tc.name, filter)) continue;
        ++ran;
        current_failed() = false;
        tc.fn();
        std::printf("[%s] %s\n", current_failed() ? "FAIL" : "ok  ", tc.name);
        if (current_failed()) ++failed;
    }
    std::printf("%zu test(s) run, %zu failed\n", ran, failed);
    return failed == 0 ? 0 : 1;
}

}  // namespace testfw

#define AX_TEST(name)                                      \
    static void name();                                    \
    static ::testfw::Registrar ax_reg_##name{#name, name}; \
    static void name()

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("  CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ::testfw::current_failed() = true;                                  \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NE(a, b) CHECK((a) != (b))

#define AX_TEST_MAIN int main(int argc, char** argv) { return ::testfw::run_all(argc, argv); }