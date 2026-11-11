slrr_add_test(
  NAME slrr_vm_dyno_tests
  SOURCES
    test_dyno_model.cpp
    ../src/jvm/gc.cpp
    ../src/jvm/jvm.cpp
    ../src/jvm/tufa.cpp
    ../src/natives/native_registry.cpp
    ../src/physics/dyno_model.cpp
  INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}/include"
)
