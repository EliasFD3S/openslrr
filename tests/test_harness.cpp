#include "test_harness.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace slrr::test {
namespace {

struct TestCase {
    std::string name;
    TestFunction function;
};

std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

} // namespace

Registrar::Registrar(std::string_view name, TestFunction function)
{
    registry().push_back(TestCase{std::string(name), function});
}

AssertionFailure::AssertionFailure(std::string_view expression, std::string_view file, int line)
    : expression_(expression), file_(file), line_(line)
{
}

const char* AssertionFailure::expression() const noexcept
{
    return expression_.data();
}

const char* AssertionFailure::file() const noexcept
{
    return file_.data();
}

int AssertionFailure::line() const noexcept
{
    return line_;
}

int run_all_tests()
{
    if (registry().empty()) {
        std::cerr << "No tests registered\n";
        return 1;
    }

    int failures = 0;
    std::cout << "Running " << registry().size() << " test(s)\n";

    for (const TestCase& test : registry()) {
        std::cout << "[ RUN      ] " << test.name << '\n';
        try {
            test.function();
            std::cout << "[       OK ] " << test.name << '\n';
        } catch (const AssertionFailure& failure) {
            ++failures;
            std::cerr << "[  FAILED  ] " << test.name << '\n'
                      << failure.file() << ':' << failure.line()
                      << ": SLRR_REQUIRE(" << failure.expression() << ") failed\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[  FAILED  ] " << test.name
                      << ": unexpected exception: " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[  FAILED  ] " << test.name
                      << ": unexpected non-standard exception\n";
        }
    }

    std::cout << (registry().size() - static_cast<std::size_t>(failures))
              << " test(s) passed; " << failures << " test(s) failed\n";
    return failures == 0 ? 0 : 1;
}

} // namespace slrr::test

int main()
{
    return slrr::test::run_all_tests();
}
