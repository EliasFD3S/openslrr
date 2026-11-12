slrr_add_test(
  NAME pedestrian_tests
  SOURCES
    test_pedestrian.cpp
    "${PROJECT_SOURCE_DIR}/src/resources/pedestrian_lines.cpp"
    "${PROJECT_SOURCE_DIR}/src/formats/walk.cpp"
  INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/include"
)
