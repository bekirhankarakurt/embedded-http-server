// Runs all registered tests, or only the suites named on the command line.
#include "test.hpp"

#include <exception>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::vector<std::string> suites(argv + 1, argv + argc);
    int run = 0;
    for (const auto& c : test::registry()) {
        bool selected = suites.empty();
        for (const auto& s : suites) {
            selected = selected || s == c.suite;
        }
        if (!selected) {
            continue;
        }
        ++run;
        const int before = test::failures();
        try {
            c.body();
        } catch (const test::Abort&) {
        } catch (const std::exception& e) {
            test::report(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        }
        std::fprintf(stderr, "[%s] %s.%s\n", test::failures() == before ? " OK " : "FAIL", c.suite.c_str(),
                     c.name.c_str());
    }
    if (run == 0) {
        std::fprintf(stderr, "no tests matched\n");
        return 1;
    }
    std::fprintf(stderr, "%d test(s), %d failure(s)\n", run, test::failures());
    return test::failures() == 0 ? 0 : 1;
}
