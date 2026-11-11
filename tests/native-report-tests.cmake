slrr_add_test(
  NAME slrr_native_report_tests
  SOURCES
    test_native_report.cpp
    "${PROJECT_SOURCE_DIR}/src/natives/native_registry.cpp"
  INCLUDE_DIRS
    "${PROJECT_SOURCE_DIR}/include"
)

find_program(SLRR_NODE_EXECUTABLE NAMES node node.exe)
if(SLRR_NODE_EXECUTABLE)
  add_test(
    NAME slrr_native_coverage_generator_tests
    COMMAND "${SLRR_NODE_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/render-native-coverage.js" --self-test
  )
  add_test(
    NAME slrr_native_coverage_drift
    COMMAND "${SLRR_NODE_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/render-native-coverage.js" --check
  )
endif()
