slrr_add_test(
  NAME slrr_native_report_tests
  SOURCES
    test_native_report.cpp
    "${PROJECT_SOURCE_DIR}/src/natives/native_registry.cpp"
  INCLUDE_DIRS
    "${PROJECT_SOURCE_DIR}/include"
)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME slrr_native_coverage_drift
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/render_native_coverage.py" --check
  )
endif()
