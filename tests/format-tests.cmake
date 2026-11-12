slrr_add_test(
  NAME format_tests
  SOURCES
    test_formats.cpp
    "${PROJECT_SOURCE_DIR}/src/formats/rest.cpp"
    "${PROJECT_SOURCE_DIR}/src/formats/trc.cpp"
  INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include"
)
