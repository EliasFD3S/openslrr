# Process capture is tested out-of-process so intentional crashes never reach
# the CTest/coordinator process. diagnostics.cpp and metrics.cpp are supplied by
# the existing instrumentation fragment in the combined test tree.
target_sources(slrr_test_support PRIVATE
  "${PROJECT_SOURCE_DIR}/src/platform/crash_logging.cpp"
)
target_include_directories(slrr_test_support PRIVATE
  "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(slrr_test_support PUBLIC dbghelp)

add_executable(slrr_crash_logging_fixture
  crash_logging_fixture.cpp
)
target_link_libraries(slrr_crash_logging_fixture PRIVATE slrr_test_support)
target_include_directories(slrr_crash_logging_fixture PRIVATE
  "${PROJECT_SOURCE_DIR}/include"
)
target_compile_features(slrr_crash_logging_fixture PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(slrr_crash_logging_fixture PRIVATE /W4 /utf-8)
endif()
add_dependencies(slrr_tests slrr_crash_logging_fixture)

slrr_add_test(
  NAME slrr_crash_logging_tests
  SOURCES test_crash_logging.cpp
  INCLUDE_DIRS
    "${PROJECT_SOURCE_DIR}/include"
)
add_dependencies(slrr_crash_logging_tests slrr_crash_logging_fixture)
