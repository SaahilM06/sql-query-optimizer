#pragma once

// Minimal header-only test framework -- no external dependencies, mirroring
// the shape of Rust's `#[test]` + `cargo test`: declare a test with TEST(name),
// use ASSERT_* inside it, and run_all_tests() discovers + runs everything
// registered across every translation unit that includes this header.

#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace sql::testing {

struct AssertionFailure : std::exception {
    std::string message;
    AssertionFailure(const std::string& expr, const char* file, int line, const std::string& note = "") {
        message = std::string(file) + ":" + std::to_string(line) + ": assertion failed: " + expr;
        if (!note.empty()) message += "  (" + note + ")";
    }
    const char* what() const noexcept override { return message.c_str(); }
};

using TestFn = void (*)();

struct TestCase {
    std::string name;
    TestFn fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline bool register_test(const std::string& name, TestFn fn) {
    registry().push_back(TestCase{name, fn});
    return true;
}

inline int run_all_tests() {
    int passed = 0;
    int failed = 0;
    for (const auto& tc : registry()) {
        try {
            tc.fn();
            std::cout << "[PASS] " << tc.name << "\n";
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << tc.name << " -- " << e.what() << "\n";
            ++failed;
        } catch (...) {
            std::cout << "[FAIL] " << tc.name << " -- unknown exception\n";
            ++failed;
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed, " << registry().size() << " total\n";
    return failed == 0 ? 0 : 1;
}

} // namespace sql::testing

#define TEST(name)                                                                 \
    static void name();                                                           \
    static const bool name##_registered = ::sql::testing::register_test(#name, name); \
    static void name()

#define ASSERT_TRUE(cond)                                                          \
    do {                                                                           \
        if (!(cond)) throw ::sql::testing::AssertionFailure(#cond, __FILE__, __LINE__); \
    } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b)                                                            \
    do {                                                                           \
        if (!((a) == (b))) throw ::sql::testing::AssertionFailure(#a " == " #b, __FILE__, __LINE__); \
    } while (0)

#define ASSERT_TRUE_MSG(cond, msg)                                                 \
    do {                                                                           \
        if (!(cond)) throw ::sql::testing::AssertionFailure(#cond, __FILE__, __LINE__, msg); \
    } while (0)

#define ASSERT_EQ_MSG(a, b, msg)                                                   \
    do {                                                                           \
        if (!((a) == (b))) throw ::sql::testing::AssertionFailure(#a " == " #b, __FILE__, __LINE__, msg); \
    } while (0)

#define FAIL_TEST(msg) throw ::sql::testing::AssertionFailure("explicit failure", __FILE__, __LINE__, msg)
