slrr_add_test(
  NAME skeleton_tests
  SOURCES
    test_skeleton.cpp
    "${PROJECT_SOURCE_DIR}/src/resources/skeleton.cpp"
    "${PROJECT_SOURCE_DIR}/src/resources/scx_mesh.cpp"
    "${PROJECT_SOURCE_DIR}/src/formats/skeleton_text.cpp"
    "${PROJECT_SOURCE_DIR}/src/formats/matrixdata.cpp"
  INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include"
)
