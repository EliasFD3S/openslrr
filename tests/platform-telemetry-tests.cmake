slrr_add_test(
  NAME slrr_platform_telemetry_tests
  SOURCES
    test_platform_telemetry.cpp
    "${PROJECT_SOURCE_DIR}/src/platform/platform_telemetry.cpp"
    "${PROJECT_SOURCE_DIR}/src/platform/metrics.cpp"
    "${PROJECT_SOURCE_DIR}/src/platform/diagnostics.cpp"
  INCLUDE_DIRS
    "${PROJECT_SOURCE_DIR}/include"
)
