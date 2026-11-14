slrr_add_test(
  NAME slrr_fuzz_arraycopy
  SOURCES
    test_arraycopy_fuzz.cpp
    test_fuzz_support.cpp
  INCLUDE_DIRS
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src/natives"
)

slrr_add_test(
  NAME slrr_fuzz_ptx
  SOURCES
    test_ptx_fuzz.cpp
    test_fuzz_support.cpp
    "${PROJECT_SOURCE_DIR}/src/resources/dds_image.cpp"
    "${PROJECT_SOURCE_DIR}/src/resources/ptx_image.cpp"
  INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include"
)

slrr_add_test(
  NAME slrr_fuzz_rpk
  SOURCES
    test_rpk_fuzz.cpp
    test_fuzz_support.cpp
    "${PROJECT_SOURCE_DIR}/src/resources/rpk_index.cpp"
  INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include"
)

slrr_add_test(
  NAME slrr_fuzz_tufa
  SOURCES
    test_tufa_fuzz.cpp
    test_fuzz_support.cpp
    "${PROJECT_SOURCE_DIR}/src/jvm/tufa.cpp"
  INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include"
)
