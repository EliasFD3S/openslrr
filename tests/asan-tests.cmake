# Apply ASan after all tests/*-tests.cmake fragments have registered their targets.
if(SLRR_ENABLE_ASAN)
  cmake_language(DEFER CALL slrr_enable_asan_for_directory_targets
                 "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
