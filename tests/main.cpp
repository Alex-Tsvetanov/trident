#include "harness.hpp"

#include <cstring>

namespace testing {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

void fail_at(const char* file, int line, const std::string& text) {
    std::ostringstream out;
    out << file << ":" << line << ": " << text;
    throw Failure(out.str());
}

int run(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    if (filter && std::strcmp(filter, "--list") == 0) {
        for (const TestCase& t : registry()) std::cout << t.group << "." << t.name << "\n";
        return 0;
    }

    int passed = 0, failed = 0, skipped = 0;
    for (const TestCase& test : registry()) {
        if (filter && test.group != filter) {
            ++skipped;
            continue;
        }
        try {
            test.body();
            std::cout << "  ok   " << test.group << "." << test.name << "\n";
            ++passed;
        } catch (const Failure& f) {
            std::cout << "  FAIL " << test.group << "." << test.name << "\n       " << f.what()
                      << "\n";
            ++failed;
        } catch (const std::exception& e) {
            std::cout << "  FAIL " << test.group << "." << test.name
                      << "\n       unexpected exception: " << e.what() << "\n";
            ++failed;
        }
    }
    std::cout << (filter ? filter : "all") << ": " << passed << " passed, " << failed
              << " failed";
    if (skipped) std::cout << ", " << skipped << " not in this group";
    std::cout << "\n";
    if (passed == 0 && failed == 0) {
        std::cout << "no test case matched the filter, which is itself a failure\n";
        return 2;
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace testing

int main(int argc, char** argv) { return testing::run(argc, argv); }
