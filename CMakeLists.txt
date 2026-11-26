cmake_minimum_required(VERSION 3.21)
project(slrr_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
   NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
  message(FATAL_ERROR "Open SLRR requires Windows x64 with an MSVC-compatible compiler. Use -A x64 with Visual Studio 2022.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake/CMakeLists.txt")
  message(FATAL_ERROR "Missing third_party/bgfx.cmake. See README.md for setup.")
endif()

# Visual Studio compiles the files of one project one after the other unless
# /MP is set, which left most cores idle. It covers bgfx.cmake's targets too.
# SLRR_COMPILE_JOBS caps the cl.exe processes per project; empty uses every core.
set(SLRR_COMPILE_JOBS "" CACHE STRING "cl.exe processes per project (/MP<n>); empty uses all logical processors")
string(APPEND CMAKE_C_FLAGS " /MP${SLRR_COMPILE_JOBS}")
string(APPEND CMAKE_CXX_FLAGS " /MP${SLRR_COMPILE_JOBS}")

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

# Physics Lab (#380): offline vehicle-solver bench, any solver rate.
add_executable(physics_lab tools/physics_lab.cpp
  src/physics/vehicle_dynamics.cpp src/physics/tire_model.cpp src/physics/steering_feedback.cpp
  src/physics/brake_system.cpp src/physics/tyre_thermal.cpp
  src/physics/suspension_geometry.cpp src/physics/dyno_model.cpp)
target_include_directories(physics_lab PRIVATE include)
target_compile_options(physics_lab PRIVATE /utf-8)

add_executable(slrr_host
  src/main.cpp
  src/jvm/script_compiler.cpp
  src/platform/game_root.cpp
  src/platform/frame_profiler.cpp
  src/platform/host_input.cpp
  src/platform/host_input_devices.cpp
  src/platform/host_clipboard.cpp
  src/platform/host_audio.cpp
  src/platform/audio_reverb.cpp
  src/platform/host_sfx.cpp
  src/platform/host_music.cpp
  src/platform/host_game_options.cpp
  src/platform/host_options_json.cpp
  src/platform/host_cursor.cpp
  src/platform/job_system.cpp
  src/platform/video_cinepak.cpp
  src/platform/video_fmv.cpp
  src/platform/video_fmv_vlc.cpp
  src/natives/native_registry.cpp
  src/natives/natives_boot.cpp
  src/natives/natives_io.cpp
  src/natives/save_json.cpp
  src/natives/natives_lang.cpp
  src/natives/natives_osd.cpp
  src/natives/natives_physicsref.cpp
  src/natives/natives_gameref.cpp
  src/natives/natives_renderref.cpp
  src/natives/natives_resref.cpp
  src/natives/natives_script.cpp
  src/natives/natives_painter.cpp
  src/natives/natives_lights.cpp
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
  src/resources/skid_marks.cpp
  src/resources/pedestrian_lines.cpp
  src/formats/walk.cpp
  src/formats/rest.cpp
  src/formats/trc.cpp
  src/formats/skeleton_text.cpp
  src/formats/matrixdata.cpp
  src/resources/gameref.cpp
  src/resources/script_lights.cpp
  src/resources/light_resource.cpp
  src/resources/ground_surface.cpp
  src/resources/scene_instances.cpp
  src/resources/scene_ambience.cpp
  src/resources/scene_triggers.cpp
  src/resources/trigger_volume.cpp
  src/resources/road_routes.cpp
  src/resources/race_runtime.cpp
  src/resources/race_brain.cpp
  src/resources/traffic_runtime.cpp
  src/resources/scx_mesh.cpp
  src/resources/skeleton.cpp
  src/resources/character.cpp
  src/resources/dds_image.cpp
  src/resources/ptx_image.cpp
  src/resources/paint_texture.cpp
  src/jvm/tufa.cpp
  src/jvm/tree_eval.cpp
  src/jvm/jvm.cpp
  src/jvm/gc.cpp
  src/jvm/script_threads.cpp
  src/boot/section_machine.cpp
  src/boot/mainloop.cpp
  src/boot/script_events.cpp
  src/boot/boot.cpp
  src/boot/drive_path.cpp
  src/physics/sl2_pacejka.cpp
  src/physics/tire_model.cpp
  src/physics/steering_feedback.cpp
  src/physics/dyno_model.cpp
  src/physics/vehicle_dynamics.cpp
  src/physics/brake_system.cpp
  src/physics/tyre_thermal.cpp
  src/physics/vehicle_contacts.cpp
  src/physics/scene_collision.cpp
  src/physics/deformation_cage.cpp
  src/physics/mass_properties.cpp
  src/physics/wheel_mount.cpp
  src/physics/suspension_geometry.cpp
  src/physics/flap_hinge.cpp
)

if(WIN32)
  target_sources(slrr_host PRIVATE slrr_host.rc)
endif()

# Refresh Git metadata before any host source compiles on every generator.
# write_version.cmake preserves the header timestamp when its content is unchanged.
set(SLRR_VERSION_H "${CMAKE_BINARY_DIR}/generated/slrr_version.h")
add_custom_target(slrr_version ALL
  BYPRODUCTS "${SLRR_VERSION_H}"
  COMMAND "${CMAKE_COMMAND}"
          "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}"
          "-DOUT=${SLRR_VERSION_H}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/write_version.cmake"
  VERBATIM)
add_dependencies(slrr_host slrr_version)
# Rebuild the window-title consumer in the same invocation when Git metadata changes.
set_property(SOURCE src/boot/boot.cpp APPEND PROPERTY
  OBJECT_DEPENDS "${SLRR_VERSION_H}")

target_include_directories(slrr_host PRIVATE include "${CMAKE_BINARY_DIR}/generated")

# Vehicle solver rate (#380): 1000 Hz on the free-running physics thread.
# --physics-hz changes it at run time; any rate from 60 Hz works (a 60 Hz host
# tick runs floor((n+1) f / 60) - floor(n f / 60) solver steps when frame-locked).
set(SLRR_VEHICLE_PHYSICS_HZ 1000 CACHE STRING "Vehicle solver rate (Hz)")
if(SLRR_VEHICLE_PHYSICS_HZ LESS 60)
  message(FATAL_ERROR "SLRR_VEHICLE_PHYSICS_HZ must be at least 60")
endif()
target_compile_definitions(slrr_host PRIVATE SLRR_VEHICLE_PHYSICS_HZ=${SLRR_VEHICLE_PHYSICS_HZ})

# The standard headers most sources include, parsed once instead of per file.
target_precompile_headers(slrr_host PRIVATE
  <algorithm> <array> <chrono> <cmath> <cstdint> <cstdio> <cstring> <filesystem>
  <fstream> <functional> <memory> <optional> <string> <unordered_map> <vector>)

if(MSVC)
  target_compile_options(slrr_host PRIVATE /W3 /utf-8)
  target_compile_definitions(slrr_host PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(WIN32)
  # strmiids = DirectShow GUIDs for FMV (GfxEngine.openVideo).
  # vfw32 = AVIFile fallback; stock Cinepak PCM is mixed through SDL3_mixer.
  target_link_libraries(slrr_host PRIVATE user32 gdi32 opengl32 winmm ole32
                                          oleaut32 strmiids vfw32)
endif()

# SDL handles peripherals and audio output; keep the existing Win32 window and renderer.
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO ON CACHE BOOL "" FORCE)
# Haptics drive force feedback wheels (DirectInput constant force and damper).
set(SDL_HAPTIC ON CACHE BOOL "" FORCE)
foreach(_slrr_sdl_subsystem IN ITEMS VIDEO GPU RENDER CAMERA POWER SENSOR DIALOG)
  set(SDL_${_slrr_sdl_subsystem} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(SDL3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
  URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f)
# SDL gates this probe on audio/video's SDL_DIRECTX option. Keep the explicit
# check so wheels, pedals and shifters never silently lose DirectInput.
include(CheckIncludeFileCXX)
check_include_file_cxx(dinput.h HAVE_DINPUT_H)
if(NOT HAVE_DINPUT_H)
  message(FATAL_ERROR "SDL peripheral input requires dinput.h from the Windows SDK.")
endif()
FetchContent_MakeAvailable(SDL3)
target_link_libraries(slrr_host PRIVATE SDL3::SDL3-static)

# SDL3_mixer mixes effects, music and stock Cinepak PCM. Only its built-in
# WAV and dr_mp3 decoders are used: the release tarball has no vendored
# libraries, and the game ships WAV effects and MP3/WAV music.
set(SDLMIXER_VENDORED OFF CACHE BOOL "" FORCE)
set(SDLMIXER_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLMIXER_TESTS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDLMIXER_DEPS_SHARED OFF CACHE BOOL "" FORCE)
foreach(_slrr_mixer_codec IN ITEMS AIFF VOC AU FLAC GME MOD MIDI OPUS VORBIS_STB
    VORBIS_VORBISFILE VORBIS_TREMOR WAVPACK MP3_MPG123)
  set(SDLMIXER_${_slrr_mixer_codec} OFF CACHE BOOL "" FORCE)
endforeach()
set(SDLMIXER_WAVE ON CACHE BOOL "" FORCE)
set(SDLMIXER_MP3 ON CACHE BOOL "" FORCE)
set(SDLMIXER_MP3_DRMP3 ON CACHE BOOL "" FORCE)
# SDL3_mixer follows BUILD_SHARED_LIBS; build it static without changing the
# value seen by later subprojects.
set(_slrr_shared_libs "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(SDL3_mixer
  URL https://github.com/libsdl-org/SDL_mixer/releases/download/release-3.2.4/SDL3_mixer-3.2.4.tar.gz
  URL_HASH SHA256=182a07c745375e113dc740d43964ff21b0be29f29f59876c4dbc4db3d32f6901)
FetchContent_MakeAvailable(SDL3_mixer)
set(BUILD_SHARED_LIBS "${_slrr_shared_libs}")
target_link_libraries(slrr_host PRIVATE SDL3_mixer::SDL3_mixer)

# bgfx is the game's renderer.
set(BGFX_BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
set(BGFX_CUSTOM_TARGETS OFF CACHE BOOL "" FORCE)

# shaderc (with glslang, SPIRV-Tools and Tint) is most of bgfx's build time and
# is identical for every checkout. When SLRR_TOOLS_CACHE_DIR (or the environment
# variable of that name) is set, it is built once into that directory and every
# checkout reuses it. Otherwise shaderc is built in this build tree.
set(SLRR_TOOLS_CACHE_DIR "" CACHE PATH "Directory shared between checkouts for prebuilt tools (shaderc); empty builds them locally")
if(NOT SLRR_TOOLS_CACHE_DIR AND DEFINED ENV{SLRR_TOOLS_CACHE_DIR})
  set(SLRR_TOOLS_CACHE_DIR "$ENV{SLRR_TOOLS_CACHE_DIR}")
endif()
if(SLRR_TOOLS_CACHE_DIR)
  set(BGFX_BUILD_TOOLS_SHADER OFF CACHE BOOL "" FORCE)
else()
  set(BGFX_BUILD_TOOLS_SHADER ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(third_party/bgfx.cmake EXCLUDE_FROM_ALL)

if(SLRR_TOOLS_CACHE_DIR)
  # Key the cached shaderc by every input that changes it: the revisions and
  # uncommitted changes of bgfx.cmake, bgfx, bimg and bx, the compiler and the
  # toolset. Sources that are not git checkouts cannot be keyed, so shaderc is
  # then built once inside this build tree instead of the shared directory.
  find_package(Git QUIET)
  set(_slrr_shaderc_key "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} ${CMAKE_GENERATOR_TOOLSET}")
  foreach(_slrr_dep IN ITEMS BGFX BIMG BX)
    get_directory_property(${_slrr_dep}_DIR
      DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake"
      DEFINITION ${_slrr_dep}_DIR)
  endforeach()
  set(_slrr_shaderc_keyed TRUE)
  foreach(_slrr_path IN ITEMS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake" "${BGFX_DIR}" "${BIMG_DIR}" "${BX_DIR}")
    set(_slrr_rev "")
    set(_slrr_rev_result 1)
    if(GIT_FOUND)
      execute_process(COMMAND "${GIT_EXECUTABLE}" --no-optional-locks -C "${_slrr_path}" rev-parse HEAD
        OUTPUT_VARIABLE _slrr_rev RESULT_VARIABLE _slrr_rev_result OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      execute_process(COMMAND "${GIT_EXECUTABLE}" --no-optional-locks -C "${_slrr_path}" diff HEAD
        OUTPUT_VARIABLE _slrr_changes ERROR_QUIET)
      execute_process(COMMAND "${GIT_EXECUTABLE}" --no-optional-locks -C "${_slrr_path}" ls-files --others --exclude-standard
        OUTPUT_VARIABLE _slrr_untracked ERROR_QUIET)
      string(SHA256 _slrr_changes "${_slrr_changes}${_slrr_untracked}")
    endif()
    if(NOT _slrr_rev_result EQUAL 0 OR _slrr_rev STREQUAL "")
      set(_slrr_shaderc_keyed FALSE)
      break()
    endif()
    string(APPEND _slrr_shaderc_key " ${_slrr_rev} ${_slrr_changes}")
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/shaderc_cache/CMakeLists.txt")
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/cmake/shaderc_cache/CMakeLists.txt" _slrr_mini_hash)
  string(SHA256 _slrr_shaderc_key "${_slrr_shaderc_key} ${_slrr_mini_hash}")
  string(SUBSTRING "${_slrr_shaderc_key}" 0 16 _slrr_shaderc_key)
  if(_slrr_shaderc_keyed)
    set(_slrr_shaderc_dir "${SLRR_TOOLS_CACHE_DIR}/shaderc-${_slrr_shaderc_key}")
  else()
    message(WARNING "bgfx sources are not git checkouts: building shaderc once in this build tree instead of ${SLRR_TOOLS_CACHE_DIR}")
    set(_slrr_shaderc_dir "${CMAKE_BINARY_DIR}/shaderc-local")
  endif()
  file(TO_CMAKE_PATH "${_slrr_shaderc_dir}" _slrr_shaderc_dir)

  add_custom_target(slrr_shaderc
    BYPRODUCTS "${_slrr_shaderc_dir}/bin/shaderc.exe"
    COMMAND "${CMAKE_COMMAND}"
            "-DSHADERC_DIR=${_slrr_shaderc_dir}"
            "-DBGFX_CMAKE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake"
            "-DBGFX_DIR=${BGFX_DIR}" "-DBIMG_DIR=${BIMG_DIR}" "-DBX_DIR=${BX_DIR}"
            "-DMINI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/cmake/shaderc_cache"
            "-DGENERATOR=${CMAKE_GENERATOR}" "-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DTOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ensure_shared_shaderc.cmake"
    VERBATIM)
  add_dependencies(slrr_host slrr_shaderc)
  # bgfxToolUtils.cmake defines its helpers only when this target exists.
  add_executable(bgfx::shaderc IMPORTED)
  set_target_properties(bgfx::shaderc PROPERTIES IMPORTED_LOCATION "${_slrr_shaderc_dir}/bin/shaderc.exe")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake/cmake/bgfxToolUtils.cmake")

set(SLRR_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
file(MAKE_DIRECTORY "${SLRR_SHADER_OUT}")
# bgfx.cmake resolves explicit directories, in-tree checkouts and FetchContent
# in its own scope. Reuse those exact paths rather than guessing _deps names.
foreach(_slrr_dep IN ITEMS BGFX BIMG)
  get_directory_property(${_slrr_dep}_DIR
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake"
    DEFINITION ${_slrr_dep}_DIR)
  if(NOT IS_DIRECTORY "${${_slrr_dep}_DIR}")
    message(FATAL_ERROR "bgfx.cmake did not resolve a valid ${_slrr_dep}_DIR")
  endif()
endforeach()
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
  SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_osd_text.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_mesh.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_mesh_inst.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_skin.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_vehicle.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_shadow.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_ao.sc"
  VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
  OUTPUT_DIR "${SLRR_SHADER_OUT}"
  INCLUDE_DIRS "${_bgfx_shader_inc}"
  AS_HEADERS
  PROFILES s_5_0 430
  OUT_FILES_VAR SLRR_VS_SHADER_HEADERS
)
bgfx_compile_shaders(
  TYPE FRAGMENT
  SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_osd_text.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_mesh.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_vehicle.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_static_shadow.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_ao.sc"
  VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
  OUTPUT_DIR "${SLRR_SHADER_OUT}"
  INCLUDE_DIRS "${_bgfx_shader_inc}"
  AS_HEADERS
  PROFILES s_5_0 430
  OUT_FILES_VAR SLRR_FS_SHADER_HEADERS
)

# bgfx's helper tracks the input and varying definition, not local includes.
# Append to the first output of each receiver's multi-profile compile rule.
foreach(_shadow_receiver IN ITEMS mesh vehicle)
  add_custom_command(
    OUTPUT "${SLRR_SHADER_OUT}/dxbc/fs_${_shadow_receiver}.sc.bin.h"
    APPEND DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_shadow.sh" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_shadow_cascade.sh" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ambient_occlusion.sh"
  )
endforeach()

target_sources(slrr_host PRIVATE
  src/render/bgfx_lib_backend.cpp
  src/render/bgfx_lighting.cpp
  src/render/bgfx_shadows.cpp
  src/render/bgfx_ao.cpp
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
message(STATUS "slrr_host: libbgfx + mesh shaders enabled")

option(BUILD_TESTING "Build regression tests" OFF)
include(CTest)
if(BUILD_TESTING)
  add_executable(vehicle_hud_damage tests/test_vehicle_hud_damage.cpp
    src/resources/scx_mesh.cpp src/platform/job_system.cpp)
  target_include_directories(vehicle_hud_damage PRIVATE include)
  target_compile_features(vehicle_hud_damage PRIVATE cxx_std_20)
  target_compile_options(vehicle_hud_damage PRIVATE /W3 /utf-8)
  add_test(NAME vehicle_hud_damage COMMAND vehicle_hud_damage)
  add_executable(audio_recovery tests/test_audio_recovery.cpp
    src/platform/host_audio.cpp src/platform/audio_reverb.cpp src/platform/host_sfx.cpp src/platform/host_music.cpp)
  target_include_directories(audio_recovery PRIVATE include)
  target_compile_options(audio_recovery PRIVATE /W3 /utf-8)
  target_compile_definitions(audio_recovery PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(audio_recovery PRIVATE SDL3_mixer::SDL3_mixer SDL3::SDL3-static)
  add_test(NAME audio_recovery COMMAND audio_recovery)
  add_executable(save_json tests/test_save_json.cpp src/natives/save_json.cpp)
  target_include_directories(save_json PRIVATE src/natives)
  target_compile_features(save_json PRIVATE cxx_std_20)
  target_compile_options(save_json PRIVATE /W3 /utf-8)
  add_test(NAME save_json COMMAND save_json)
  add_executable(float_literal_mode tests/test_float_literal_mode.cpp
    src/jvm/script_compiler.cpp src/jvm/tufa.cpp)
  target_link_libraries(float_literal_mode PRIVATE slrr_script_compiler)
  target_include_directories(float_literal_mode PRIVATE include)
  target_compile_options(float_literal_mode PRIVATE /W3 /utf-8)
  add_test(NAME float_literal_mode COMMAND float_literal_mode)
  add_executable(ground_surface tests/test_ground_surface.cpp src/resources/ground_surface.cpp)
  target_include_directories(ground_surface PRIVATE include)
  target_compile_features(ground_surface PRIVATE cxx_std_20)
  target_compile_options(ground_surface PRIVATE /W3 /utf-8)
  add_test(NAME ground_surface COMMAND ground_surface)
endif()

install(TARGETS slrr_host RUNTIME DESTINATION bin)
