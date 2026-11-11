slrr_add_test(
  NAME slrr_compiler_tests
  SOURCES test_script_compiler.cpp
    ../src/jvm/script_compiler.cpp ../src/jvm/tufa.cpp
  LIBRARIES slrr_script_compiler
  INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include")
