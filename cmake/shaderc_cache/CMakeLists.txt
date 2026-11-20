# Builds only bgfx's shaderc, so that every checkout can reuse one copy.
# Configured by cmake/ensure_shared_shaderc.cmake; not part of the game build.
cmake_minimum_required(VERSION 3.21)
project(slrr_shaderc_cache LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  string(APPEND CMAKE_C_FLAGS " /MP")
  string(APPEND CMAKE_CXX_FLAGS " /MP")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/out")

set(BGFX_BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_SHADER ON CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_BIN2C OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_GEOMETRY OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS_TEXTURE OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
set(BGFX_CUSTOM_TARGETS OFF CACHE BOOL "" FORCE)
add_subdirectory("${SLRR_BGFX_CMAKE_DIR}" bgfx.cmake EXCLUDE_FROM_ALL)
# A top-level project that Visual Studio generators can build by name.
add_custom_target(slrr_shaderc_only)
add_dependencies(slrr_shaderc_only shaderc)
