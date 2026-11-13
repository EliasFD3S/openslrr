#pragma once

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

#define SLRR_TEST(name) void name()
#define SLRR_REQUIRE(expression)                                                    \
  do {                                                                              \
    if (!(expression))                                                              \
      throw std::runtime_error(std::string(#expression) + " at " + __FILE__ +      \
                               ":" + std::to_string(__LINE__));                     \
  } while (false)

namespace slrr::standalone {
using Test = void (*)();

template <size_t N>
int run(const char* suite, const Test (&tests)[N]) {
  int failed = 0;
  for (size_t i = 0; i < N; ++i) {
    try {
      tests[i]();
    } catch (const std::exception& error) {
      ++failed;
      std::fprintf(stderr, "[%s] test %zu failed: %s\n", suite, i + 1, error.what());
    } catch (...) {
      ++failed;
      std::fprintf(stderr, "[%s] test %zu failed with an unknown error\n", suite, i + 1);
    }
  }
  std::fprintf(stderr, "[%s] %zu passed, %d failed\n", suite, N - failed, failed);
  return failed ? 1 : 0;
}
}  // namespace slrr::standalone
