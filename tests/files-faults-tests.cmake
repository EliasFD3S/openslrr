slrr_add_test(
  NAME slrr_files_faults_tests
  SOURCES
    test_file_faults.cpp
    "${PROJECT_SOURCE_DIR}/src/natives/file_faults.cpp"
  INCLUDE_DIRS
    "${PROJECT_SOURCE_DIR}/include"
)
