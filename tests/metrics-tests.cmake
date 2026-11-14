slrr_add_test(
  NAME slrr_instrumentation_metrics_tests
  SOURCES
    test_metrics.cpp
    "${PROJECT_SOURCE_DIR}/src/jvm/gc.cpp"
    "${PROJECT_SOURCE_DIR}/src/jvm/jvm.cpp"
    "${PROJECT_SOURCE_DIR}/src/jvm/tufa.cpp"
    "${PROJECT_SOURCE_DIR}/src/natives/native_registry.cpp"
    "${PROJECT_SOURCE_DIR}/src/physics/dyno_model.cpp"
  INCLUDE_DIRS
    "${PROJECT_SOURCE_DIR}/include"
)
