// A test runner small enough to read in one sitting. Registration happens
// through a static object, so a test file needs no wiring beyond including this
// header. CTest calls the binary once per group with the group name as a filter.
#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string group;
    std::string name;
    std::function<void()> body;
};

// Failures throw, so the first failed check ends the case and the rest still run.
struct Failure : std::exception {
    std::string text;
    explicit Failure(std::string t) : text(std::move(t)) {}
    const char* what() const noexcept override { return text.c_str(); }
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* group, const char* name, std::function<void()> body) {
        registry().push_back(TestCase{group, name, std::move(body)});
    }
};

[[noreturn]] void fail_at(const char* file, int line, const std::string& text);

// Values that can be streamed get printed on failure; the rest only get named.
template <typename T>
concept Streamable = requires(std::ostream& out, const T& value) { out << value; };

template <typename T>
void describe(std::ostringstream& out, const T& value) {
    if constexpr (Streamable<T>) {
        out << value;
    } else {
        out << "<not printable>";
    }
}

template <typename A, typename B>
void check_eq(const char* file, int line, const char* ea, const char* eb, const A& a,
              const B& b) {
    if (!(a == b)) {
        std::ostringstream out;
        out << ea << " == " << eb << "\n      left:  ";
        describe(out, a);
        out << "\n      right: ";
        describe(out, b);
        fail_at(file, line, out.str());
    }
}

int run(int argc, char** argv);

}  // namespace testing

#define TRIDENT_CAT2(a, b) a##b
#define TRIDENT_CAT(a, b) TRIDENT_CAT2(a, b)

// TEST(group, name) { ... }
#define TEST(group, name)                                                            \
    static void TRIDENT_CAT(trident_test_, __LINE__)();                              \
    static ::testing::Registrar TRIDENT_CAT(trident_reg_, __LINE__)(                 \
        #group, #name, TRIDENT_CAT(trident_test_, __LINE__));                        \
    static void TRIDENT_CAT(trident_test_, __LINE__)()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) ::testing::fail_at(__FILE__, __LINE__, "CHECK(" #cond ")");      \
    } while (false)

#define CHECK_EQ(a, b) ::testing::check_eq(__FILE__, __LINE__, #a, #b, (a), (b))

#define CHECK_THROWS(expr, exception_type)                                           \
    do {                                                                             \
        bool caught = false;                                                         \
        try {                                                                        \
            (void)(expr);                                                            \
        } catch (const exception_type&) {                                            \
            caught = true;                                                           \
        } catch (...) {                                                              \
            ::testing::fail_at(__FILE__, __LINE__,                                   \
                               "expected " #exception_type " from " #expr            \
                               ", got a different exception");                       \
        }                                                                            \
        if (!caught)                                                                 \
            ::testing::fail_at(__FILE__, __LINE__,                                   \
                               "expected " #exception_type " from " #expr            \
                               ", nothing was thrown");                              \
    } while (false)
