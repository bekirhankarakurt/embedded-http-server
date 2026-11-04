// Minimal dependency-free test harness: TEST() registers a case, CHECK()/REQUIRE() assert.
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace test {

struct Case {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> body) {
        registry().push_back(Case{suite, name, std::move(body)});
    }
};

/// Thrown by REQUIRE to abort the current test case.
struct Abort : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline void report(const char* file, int line, const std::string& message) {
    ++failures();
    std::fprintf(stderr, "  %s:%d: %s\n", file, line, message.c_str());
}

template <typename T, typename = void>
struct Printable : std::false_type {};
template <typename T>
struct Printable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <typename T>
void print(std::ostream& out, const T& value) {
    if constexpr (Printable<T>::value) {
        out << value;
    } else {
        out << "<unprintable>";
    }
}

template <typename A, typename B>
std::string describe(const char* lhs, const char* rhs, const A& a, const B& b) {
    std::ostringstream out;
    out << lhs << " == " << rhs << " (";
    print(out, a);
    out << " vs ";
    print(out, b);
    out << ")";
    return out.str();
}

}  // namespace test

#define TEST_CONCAT_INNER(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_INNER(a, b)

#define TEST(suite, name)                                                                     \
    static void TEST_CONCAT(test_fn_, __LINE__)();                                            \
    static const test::Registrar TEST_CONCAT(test_reg_, __LINE__)(#suite, #name,              \
                                                                  TEST_CONCAT(test_fn_, __LINE__)); \
    static void TEST_CONCAT(test_fn_, __LINE__)()

#define CHECK(expr)                                                \
    do {                                                           \
        if (!(expr)) {                                             \
            test::report(__FILE__, __LINE__, "CHECK(" #expr ")");  \
        }                                                          \
    } while (false)

#define REQUIRE(expr)                                                  \
    do {                                                               \
        if (!(expr)) {                                                 \
            test::report(__FILE__, __LINE__, "REQUIRE(" #expr ")");    \
            throw test::Abort("requirement failed");                   \
        }                                                              \
    } while (false)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        const auto test_a_ = (a);                                                   \
        const auto test_b_ = (b);                                                   \
        if (!(test_a_ == test_b_)) {                                                 \
            test::report(__FILE__, __LINE__, test::describe(#a, #b, test_a_, test_b_)); \
        }                                                                            \
    } while (false)
