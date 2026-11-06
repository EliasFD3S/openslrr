# Host window title. The number is the commit count, so it only moves
# when the host repo does. A dirty tree keeps the same number and adds +.
find_package(Git QUIET)
set(ver "dev")
if(GIT_FOUND AND DEFINED SRC)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
    WORKING_DIRECTORY "${SRC}"
    OUTPUT_VARIABLE rev
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
    WORKING_DIRECTORY "${SRC}"
    OUTPUT_VARIABLE hash
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" status --porcelain
    WORKING_DIRECTORY "${SRC}"
    OUTPUT_VARIABLE dirty
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(rev AND hash)
    set(ver "r${rev}-${hash}")
    if(NOT dirty STREQUAL "")
      set(ver "${ver}+")
    endif()
  endif()
endif()
set(text "#pragma once\n#define SLRR_VERSION \"${ver}\"\n")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
  if(old STREQUAL text)
    return()
  endif()
endif()
file(WRITE "${OUT}" "${text}")
