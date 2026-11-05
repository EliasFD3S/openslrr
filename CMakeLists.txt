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
  src/ui/osd_script.cpp
  src/ui/ui_time.cpp
  src/render/bgfx_backend.cpp
  src/render/slrr_font.cpp
  src/resources/rpk_index.cpp
  src/resources/script_cfg.cpp
  src/resources/part_config.cpp
  src/resources/part_runtime.cpp
  src/resources/gameref.cpp
  src/resources/scene_instances.cpp
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
  src/physics/dyno_model.cpp
  src/physics/vehicle_dynamics.cpp
  src/physics/suspension_geometry.cpp
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
                                          oleaut32 strmiids vfw32 xaudio2)
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
    SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_mesh.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vs_vehicle.sc"
    VARYING_DEF "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
    OUTPUT_DIR "${SLRR_SHADER_OUT}"
    INCLUDE_DIRS "${_bgfx_shader_inc}"
    AS_HEADERS
    PROFILES s_5_0 430
    OUT_FILES_VAR SLRR_VS_SHADER_HEADERS
  )
  bgfx_compile_shaders(
    TYPE FRAGMENT
    SHADERS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_mesh.sc" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fs_vehicle.sc"
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

option(SLRR_STAGE_BUILD "Build the host into candidate/ before validation and promotion" OFF)
if(SLRR_STAGE_BUILD)
  set_target_properties(slrr_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/candidate")
endif()

option(SLRR_BUILD_TESTS "Build host UI regression tests" OFF)
if(SLRR_BUILD_TESTS)
  enable_testing()
  add_executable(ui_state_test tests/ui_state_test.cpp
    src/ui/osd_registry.cpp src/ui/ui_time.cpp src/render/slrr_font.cpp)
  target_include_directories(ui_state_test PRIVATE include)
  target_compile_features(ui_state_test PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_definitions(ui_state_test PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  add_test(NAME ui_state COMMAND ui_state_test)
  add_executable(part_config_test tests/part_config_test.cpp
    src/resources/part_config.cpp src/resources/rpk_index.cpp)
  target_include_directories(part_config_test PRIVATE include)
  target_compile_features(part_config_test PRIVATE cxx_std_20)
  add_test(NAME part_config COMMAND part_config_test)
  add_executable(vehicle_dynamics_test tests/vehicle_dynamics_test.cpp src/physics/vehicle_dynamics.cpp src/physics/suspension_geometry.cpp)
  target_include_directories(vehicle_dynamics_test PRIVATE include)
  add_test(NAME vehicle_dynamics COMMAND vehicle_dynamics_test)

  add_executable(scene_instances_test tests/scene_instances_test.cpp
    tests/scene_platform_stubs.cpp
    src/resources/scene_instances.cpp src/resources/gameref.cpp
    src/resources/rpk_index.cpp src/resources/scx_mesh.cpp)
  target_include_directories(scene_instances_test PRIVATE include)
  target_compile_features(scene_instances_test PRIVATE cxx_std_20)
  add_test(NAME scene_instances COMMAND scene_instances_test)

  # Integration checks execute the actual TUFA scripts and original assets.
  # Build a separate test executable so a clean build cannot use stale probes.
  get_target_property(_runtime_sources slrr_host SOURCES)
  list(FILTER _runtime_sources EXCLUDE REGEX "(^|/)main\\.cpp$|\\.rc$")
  add_executable(host_runtime_test tests/runtime_regression.cpp ${_runtime_sources})
  foreach(_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES LINK_LIBRARIES)
    get_target_property(_value slrr_host ${_property})
    if(_value)
      set_property(TARGET host_runtime_test PROPERTY ${_property} "${_value}")
    endif()
  endforeach()
  add_dependencies(host_runtime_test slrr_host)
  set(SLRR_TEST_GAME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../game" CACHE PATH "Original SLRR assets for integration tests")
  if(EXISTS "${SLRR_TEST_GAME_ROOT}/system/Scripts/lang/System.class")
    add_test(NAME part_config_original_assets COMMAND part_config_test "${SLRR_TEST_GAME_ROOT}")
    foreach(_case sfx_probe regression_probe gap_probe hud_probe career_probe filedialog_probe scene_script_probe icon_probe paint_inventory vehicle_assembly catalog vehicle_physics)
      add_test(NAME runtime_${_case} COMMAND host_runtime_test ${_case} "${SLRR_TEST_GAME_ROOT}")
      set_tests_properties(runtime_${_case} PROPERTIES TIMEOUT 30)
    endforeach()
    add_test(NAME scene_original_assets COMMAND scene_instances_test "${SLRR_TEST_GAME_ROOT}")
  endif()
endif()
