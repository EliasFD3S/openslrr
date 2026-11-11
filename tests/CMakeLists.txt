add_library(slrr_test_support STATIC
  test_harness.cpp
  support/test_fixtures.cpp
)
target_include_directories(slrr_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(slrr_test_support PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(slrr_test_support PRIVATE /W4 /utf-8)
endif()

add_custom_target(slrr_tests)

function(slrr_add_test)
  set(options)
  set(one_value_args NAME)
  set(multi_value_args SOURCES LIBRARIES INCLUDE_DIRS)
  cmake_parse_arguments(SLRR_TEST "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  if(NOT SLRR_TEST_NAME)
    message(FATAL_ERROR "slrr_add_test requires NAME")
  endif()
  if(NOT SLRR_TEST_SOURCES)
    message(FATAL_ERROR "slrr_add_test(${SLRR_TEST_NAME}) requires SOURCES")
  endif()
  if(SLRR_TEST_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "slrr_add_test(${SLRR_TEST_NAME}) has unknown arguments: ${SLRR_TEST_UNPARSED_ARGUMENTS}")
  endif()
  if(TARGET "${SLRR_TEST_NAME}")
    message(FATAL_ERROR "Duplicate test target: ${SLRR_TEST_NAME}")
  endif()

  add_executable("${SLRR_TEST_NAME}" ${SLRR_TEST_SOURCES})
  target_link_libraries("${SLRR_TEST_NAME}" PRIVATE slrr_test_support ${SLRR_TEST_LIBRARIES})
  target_include_directories("${SLRR_TEST_NAME}" PRIVATE ${SLRR_TEST_INCLUDE_DIRS})
  target_compile_features("${SLRR_TEST_NAME}" PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options("${SLRR_TEST_NAME}" PRIVATE /W4 /utf-8)
  endif()

  add_test(NAME "${SLRR_TEST_NAME}" COMMAND "$<TARGET_FILE:${SLRR_TEST_NAME}>")
  add_dependencies(slrr_tests "${SLRR_TEST_NAME}")
endfunction()

file(GLOB SLRR_TEST_FRAGMENTS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*-tests.cmake")
foreach(SLRR_TEST_FRAGMENT IN LISTS SLRR_TEST_FRAGMENTS)
  include("${SLRR_TEST_FRAGMENT}")
endforeach()
