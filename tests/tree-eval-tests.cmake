slrr_add_test(
  NAME slrr_tree_eval_tests
  SOURCES
    test_tree_eval.cpp
    ../src/jvm/gc.cpp
    ../src/jvm/jvm.cpp
    ../src/jvm/tree_eval.cpp
    ../src/jvm/tufa.cpp
    ../src/natives/native_registry.cpp
  INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}/include"
)
