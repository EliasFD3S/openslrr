cmake_minimum_required(VERSION 3.21)
project(slrr_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
   (MSVC AND NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64"))
  message(FATAL_ERROR "Open SLRR requires Windows x64. Use -A x64 with Visual Studio.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake/CMakeLists.txt")
  message(FATAL_ERROR "Missing third_party/bgfx.cmake. See README.md for setup.")
endif()

add_library(slrr_script_compiler STATIC
  third_party/slrrc/classes.cpp third_party/slrrc/classfile.cpp
  third_party/slrrc/decl.cpp third_party/slrrc/diag.cpp
  third_party/slrrc/emit.cpp third_party/slrrc/floatlit.cpp
  third_party/slrrc/install.cpp third_party/slrrc/packs.cpp
  third_party/slrrc/parser.cpp third_party/slrrc/pool.cpp
  third_party/slrrc/scanner.cpp third_party/slrrc/slrrc.cpp
  third_party/slrrc/write.cpp)
set_target_properties(slrr_script_compiler PROPERTIES CXX_STANDARD 17)
target_include_directories(slrr_script_compiler PUBLIC third_party/slrrc)
target_compile_options(slrr_script_compiler PRIVATE /utf-8 /fp:precise)

add_executable(compile_game_sources tools/compile_game_sources.cpp
  src/jvm/script_compiler.cpp src/jvm/tufa.cpp)
target_link_libraries(compile_game_sources PRIVATE slrr_script_compiler)
target_include_directories(compile_game_sources PRIVATE include)
target_compile_options(compile_game_sources PRIVATE /utf-8)

add_executable(slrr_host
  src/main.cpp
  src/jvm/script_compiler.cpp
  src/platform/game_root.cpp
  src/platform/frame_profiler.cpp
  src/platform/host_input.cpp
  src/platform/host_sfx.cpp
  src/platform/host_music.cpp
  src/platform/host_game_options.cpp
  src/platform/host_cursor.cpp
  src/platform/video_fmv.cpp
  src/platform/video_fmv_vlc.cpp
  src/natives/native_registry.cpp
  src/natives/natives_boot.cpp
  src/natives/natives_io.cpp
  src/natives/natives_lang.cpp
  src/natives/natives_osd.cpp
  src/natives/natives_physicsref.cpp
  src/natives/natives_gameref.cpp
  src/natives/natives_renderref.cpp
  src/natives/natives_resref.cpp
  src/natives/natives_script.cpp
  src/ui/osd_registry.cpp
  src/ui/osd_script.cpp
  src/ui/ui_time.cpp
  src/render/bgfx_backend.cpp
  src/render/slrr_font.cpp
  src/resources/rpk_index.cpp
  src/resources/script_cfg.cpp
  src/resources/scene_sun.cpp
  src/resources/part_config.cpp
  src/resources/part_runtime.cpp
  src/resources/pedestrian_lines.cpp
  src/formats/walk.cpp
  src/formats/rest.cpp
  src/formats/trc.cpp
  src/formats/skeleton_text.cpp
  src/formats/matrixdata.cpp
  src/resources/gameref.cpp
  src/resources/scene_instances.cpp
  src/resources/scene_ambience.cpp
  src/resources/scx_mesh.cpp
  src/resources/skeleton.cpp
  src/resources/character.cpp
  src/resources/dds_image.cpp
  src/resources/ptx_image.cpp
  src/jvm/tufa.cpp
  src/jvm/tree_eval.cpp
  src/jvm/jvm.cpp
  src/jvm/gc.cpp
  src/jvm/script_threads.cpp
  src/boot/section_machine.cpp
  src/boot/mainloop.cpp
  src/boot/script_events.cpp
  src/boot/boot.cpp
  src/physics/sl2_pacejka.cpp
  src/physics/tire_model.cpp
  src/physics/dyno_model.cpp
  src/physics/vehicle_dynamics.cpp
  src/physics/scene_collision.cpp
  src/physics/deformation_cage.cpp
  src/physics/suspension_geometry.cpp
)

if(WIN32)
  target_sources(slrr_host PRIVATE slrr_host.rc)
endif()

set(SLRR_VERSION_H "${CMAKE_BINARY_DIR}/generated/slrr_version.h")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
add_custom_command(
  TARGET slrr_host PRE_BUILD
  COMMAND ${CMAKE_COMMAND}
          -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT=${SLRR_VERSION_H}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_version.cmake
  VERBATIM)
execute_process(
  COMMAND ${CMAKE_COMMAND}
          -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT=${SLRR_VERSION_H}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_version.cmake)

target_include_directories(slrr_host PRIVATE include "${CMAKE_BINARY_DIR}/generated")

if(MSVC)
  target_compile_options(slrr_host PRIVATE /W3 /utf-8)
  target_compile_definitions(slrr_host PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(WIN32)
  # strmiids = DirectShow GUIDs for FMV (GfxEngine.openVideo).
  # vfw32 = Cinepak (prime.avi is cvid). The 32-bit codec does not connect
  # through the Sample Grabber graph, so menu playback uses AVIFile.
  target_link_libraries(slrr_host PRIVATE user32 gdi32 opengl32 winmm ole32 xinput
                                          oleaut32 strmiids vfw32 xaudio2 mfplat mfreadwrite mfuuid)
endif()

# bgfx is the game's renderer.
set(BGFX_BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
set(BGFX_CUSTOM_TARGETS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/bgfx.cmake EXCLUDE_FROM_ALL)
include("${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake/cmake/bgfxToolUtils.cmake")

set(SLRR_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
file(MAKE_DIRECTORY "${SLRR_SHADER_OUT}")
# Allow each architecture to use its own build tree with the same sources.
if(NOT BGFX_DIR)
  set(BGFX_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/bgfx-src")
endif()
if(NOT BIMG_DIR)
  set(BIMG_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/bimg-src")
endif()
set(_bgfx_shader_inc "${BGFX_DIR}/src")

# Use Dear ImGui and its renderer from the same bgfx checkout.
set(SLRR_IMGUI_DIR "${BGFX_DIR}/3rdparty/dear-imgui")
target_sources(slrr_host PRIVATE
  src/render/debug_overlay.cpp
  "${BGFX_DIR}/examples/common/imgui/imgui.cpp"
  "${SLRR_IMGUI_DIR}/imgui.cpp"
  "${SLRR_IMGUI_DIR}/imgui_draw.cpp"
  "${SLRR_IMGUI_DIR}/imgui_tables.cpp"
  "${SLRR_IMGUI_DIR}/imgui_widgets.cpp"
)
target_include_directories(slrr_host PRIVATE
  "${BGFX_DIR}/examples/common"
  "${BGFX_DIR}/3rdparty"
)

bgfx_compile_shaders(
  TYPE VERTEX
  SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_mesh.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_mesh_inst.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_skin.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_vehicle.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_shadow_recv.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_shadow_cast.sc"
  VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
  OUTPUT_DIR "${SLRR_SHADER_OUT}"
  INCLUDE_DIRS "${_bgfx_shader_inc}"
  AS_HEADERS
  PROFILES s_5_0 430
  OUT_FILES_VAR SLRR_VS_SHADER_HEADERS
)
bgfx_compile_shaders(
  TYPE FRAGMENT
  SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_mesh.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_vehicle.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_white.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_shadow_recv.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_static_shadow.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_present.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_hiz.sc"
  VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
  OUTPUT_DIR "${SLRR_SHADER_OUT}"
  INCLUDE_DIRS "${_bgfx_shader_inc}"
  AS_HEADERS
  PROFILES s_5_0 430
  OUT_FILES_VAR SLRR_FS_SHADER_HEADERS
)

target_sources(slrr_host PRIVATE
  src/render/bgfx_lib_backend.cpp
  src/render/bgfx_lighting.cpp
  src/render/bgfx_shadows.cpp
  src/render/bgfx_osd.cpp
  ${SLRR_VS_SHADER_HEADERS}
  ${SLRR_FS_SHADER_HEADERS}
)
set_source_files_properties(
  ${SLRR_VS_SHADER_HEADERS} ${SLRR_FS_SHADER_HEADERS}
  PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE
)
target_include_directories(slrr_host PRIVATE
  "${SLRR_SHADER_OUT}"
  "${BIMG_DIR}/3rdparty"
)
target_link_libraries(slrr_host PRIVATE bgfx bx bimg slrr_script_compiler)
target_compile_features(slrr_host PRIVATE cxx_std_20)
target_compile_definitions(slrr_host PRIVATE SLRR_HAS_LIBBGFX=1)
message(STATUS "slrr_host: libbgfx + mesh shaders enabled")

install(TARGETS slrr_host RUNTIME DESTINATION bin)
