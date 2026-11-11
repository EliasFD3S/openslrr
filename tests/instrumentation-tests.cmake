# diagnostics.cpp also satisfies the existing VM lane's native/GC references
# without editing another lane's test fragment.
target_sources(slrr_test_support PRIVATE
  "${PROJECT_SOURCE_DIR}/src/platform/diagnostics.cpp"
  # diagnostics.cpp publishes section snapshots through the Stage 2 registry.
  "${PROJECT_SOURCE_DIR}/src/platform/metrics.cpp"
)
target_include_directories(slrr_test_support PRIVATE
  "${PROJECT_SOURCE_DIR}/include"
)

slrr_add_test(
  NAME slrr_instrumentation_diagnostics_tests
  SOURCES
    test_diagnostics.cpp
    "${PROJECT_SOURCE_DIR}/src/natives/native_registry.cpp"
  INCLUDE_DIRS
    "${PROJECT_SOURCE_DIR}/include"
)
