slrr_add_test(
  NAME slrr_physics_telemetry_tests
  SOURCES
    test_physics_telemetry.cpp
    "${PROJECT_SOURCE_DIR}/src/physics/physics_telemetry.cpp"
    "${PROJECT_SOURCE_DIR}/src/physics/vehicle_dynamics.cpp"
    "${PROJECT_SOURCE_DIR}/src/physics/suspension_geometry.cpp"
    "${PROJECT_SOURCE_DIR}/src/physics/tire_model.cpp"
  INCLUDE_DIRS
    "${PROJECT_SOURCE_DIR}/include"
)
