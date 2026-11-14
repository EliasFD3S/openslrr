slrr_add_test(
  NAME safety_arraycopy_tests
  SOURCES test_arraycopy.cpp
  INCLUDE_DIRS
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src/natives"
)

slrr_add_test(
  NAME safety_ptx_image_tests
  SOURCES
    test_ptx_image.cpp
    "${PROJECT_SOURCE_DIR}/src/resources/dds_image.cpp"
    "${PROJECT_SOURCE_DIR}/src/resources/ptx_image.cpp"
  INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include"
)
