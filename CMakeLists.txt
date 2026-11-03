cmake_minimum_required(VERSION 3.21)
project(slrr_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake/CMakeLists.txt")
  option(SLRR_WITH_LIBBGFX "Fetch/build libbgfx + mesh shaders" ON)
else()
  option(SLRR_WITH_LIBBGFX "Fetch/build libbgfx + mesh shaders" OFF)
endif()

add_executable(slrr_host
  src/main.cpp
  src/platform/game_root.cpp
  src/platform/host_input.cpp
  src/platform/host_sfx.cpp
  src/platform/host_cursor.cpp
  src/platform/garage_camera.cpp
  src/platform/video_fmv.cpp
  src/natives/native_registry.cpp
  src/natives/natives_boot.cpp
  src/natives/natives_io.cpp
  src/natives/natives_lang.cpp
  src/natives/natives_osd.cpp
  src/natives/natives_gameref.cpp
  src/natives/natives_renderref.cpp
  src/natives/natives_resref.cpp
  src/natives/natives_script.cpp
  src/ui/osd_registry.cpp
  src/render/bgfx_backend.cpp
  src/render/slrr_font.cpp
  src/resources/rpk_index.cpp
  src/resources/script_cfg.cpp
  src/resources/gameref.cpp
  src/resources/scx_mesh.cpp
  src/resources/dds_image.cpp
  src/resources/ptx_image.cpp
  src/jvm/tufa.cpp
  src/jvm/tree_eval.cpp
  src/jvm/jvm.cpp
  src/boot/section_machine.cpp
  src/boot/mainloop.cpp
  src/boot/boot.cpp
  src/physics/sl2_pacejka.cpp
)

if(WIN32)
  target_sources(slrr_host PRIVATE slrr_host.rc)
endif()

target_include_directories(slrr_host PRIVATE include)

if(MSVC)
  target_compile_options(slrr_host PRIVATE /W3 /utf-8)
  target_compile_definitions(slrr_host PRIVATE _CRT_SECURE_NO_WARNINGS)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "slrr_host: building x64 — stock scripts assume 32-bit layouts; prefer -A Win32 for now")
  endif()
endif()

if(WIN32)
  # strmiids = DirectShow GUIDs for FMV (GfxEngine.openVideo).
  # vfw32 = Cinepak (prime.avi is cvid). The 32-bit codec does not connect
  # through the Sample Grabber graph, so menu playback uses AVIFile.
  target_link_libraries(slrr_host PRIVATE user32 gdi32 opengl32 winmm ole32
                                          oleaut32 strmiids vfw32)
endif()

if(SLRR_WITH_LIBBGFX)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake/CMakeLists.txt")
    message(FATAL_ERROR "SLRR_WITH_LIBBGFX=ON but third_party/bgfx.cmake missing")
  endif()
  set(CMAKE_CXX_STANDARD 20)
  set(BGFX_BUILD_TOOLS ON CACHE BOOL "" FORCE)
  set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BGFX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
  set(BGFX_CUSTOM_TARGETS OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/bgfx.cmake EXCLUDE_FROM_ALL)
  include("${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake/cmake/bgfxToolUtils.cmake")

  set(SLRR_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
  file(MAKE_DIRECTORY "${SLRR_SHADER_OUT}")
  set(_bgfx_shader_inc "${CMAKE_CURRENT_BINARY_DIR}/_deps/bgfx-src/src")

  bgfx_compile_shaders(
    TYPE VERTEX
    SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_mesh.sc"
    VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
    OUTPUT_DIR "${SLRR_SHADER_OUT}"
    INCLUDE_DIRS "${_bgfx_shader_inc}"
    AS_HEADERS
    PROFILES s_5_0 430
    OUT_FILES_VAR SLRR_VS_SHADER_HEADERS
  )
  bgfx_compile_shaders(
    TYPE FRAGMENT
    SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_mesh.sc"
    VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
    OUTPUT_DIR "${SLRR_SHADER_OUT}"
    INCLUDE_DIRS "${_bgfx_shader_inc}"
    AS_HEADERS
    PROFILES s_5_0 430
    OUT_FILES_VAR SLRR_FS_SHADER_HEADERS
  )

  target_sources(slrr_host PRIVATE
    src/render/bgfx_lib_backend.cpp
    ${SLRR_VS_SHADER_HEADERS}
    ${SLRR_FS_SHADER_HEADERS}
  )
  set_source_files_properties(
    ${SLRR_VS_SHADER_HEADERS} ${SLRR_FS_SHADER_HEADERS}
    PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE
  )
  target_include_directories(slrr_host PRIVATE
    "${SLRR_SHADER_OUT}"
    "${CMAKE_CURRENT_BINARY_DIR}/_deps/bimg-src/3rdparty"
  )
  target_link_libraries(slrr_host PRIVATE bgfx bx bimg)
  target_compile_features(slrr_host PRIVATE cxx_std_20)
  target_compile_definitions(slrr_host PRIVATE SLRR_HAS_LIBBGFX=1)
  message(STATUS "slrr_host: libbgfx + mesh shaders enabled")
endif()

install(TARGETS slrr_host RUNTIME DESTINATION bin)
