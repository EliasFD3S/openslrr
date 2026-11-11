# Opt-in target-scoped MSVC AddressSanitizer support. The normal Release
# configuration does not call the instrumentation branch unless explicitly enabled.
function(slrr_enable_asan target_name)
  if(NOT SLRR_ENABLE_ASAN)
    return()
  endif()
  if(NOT MSVC)
    message(FATAL_ERROR "SLRR_ENABLE_ASAN currently supports MSVC only")
  endif()
  if(NOT TARGET "${target_name}")
    message(FATAL_ERROR "slrr_enable_asan: missing target ${target_name}")
  endif()

  get_target_property(target_type "${target_name}" TYPE)
  if(NOT target_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY)$")
    return()
  endif()
  target_compile_options("${target_name}" PRIVATE /fsanitize=address /Zi)
  if(NOT target_type STREQUAL "STATIC_LIBRARY")
    # cl.exe embeds the ASan runtime linker directives from the compile flag.
    target_link_options("${target_name}" PRIVATE /INCREMENTAL:NO)
  endif()
endfunction()

# tests/asan-tests.cmake defers this until every uniquely owned test fragment has
# created its targets. This avoids editing the shared test registration harness.
function(slrr_enable_asan_for_directory_targets directory)
  if(NOT SLRR_ENABLE_ASAN)
    return()
  endif()
  get_property(directory_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target_name IN LISTS directory_targets)
    slrr_enable_asan("${target_name}")
  endforeach()
endfunction()
