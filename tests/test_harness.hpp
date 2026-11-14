#ifndef SLRR_TEST_HARNESS_HPP
#define SLRR_TEST_HARNESS_HPP

#include <string_view>

namespace slrr::test {

using TestFunction = void (*)();

class Registrar {
public:
    Registrar(std::string_view name, TestFunction function);
};

class AssertionFailure {
public:
    AssertionFailure(std::string_view expression, std::string_view file, int line);

    const char* expression() const noexcept;
    const char* file() const noexcept;
    int line() const noexcept;

private:
    std::string_view expression_;
    std::string_view file_;
    int line_;
};

int run_all_tests();

} // namespace slrr::test

#define SLRR_TEST(function_name)                                                            static void function_name();                                                            static const ::slrr::test::Registrar slrr_registrar_##function_name(                        #function_name, &function_name);                                                     static void function_name()

#define SLRR_REQUIRE(expression)                                                            do {                                                                                        if (!(expression)) {                                                                        throw ::slrr::test::AssertionFailure(#expression, __FILE__, __LINE__);               }                                                                                   } while (false)

#endif
