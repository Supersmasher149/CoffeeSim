add_library(catch2_amalgamated STATIC
  ${PROJECT_SOURCE_DIR}/third_party/catch2/catch_amalgamated.cpp)
target_include_directories(catch2_amalgamated SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/catch2)

add_executable(espressolab_tests
  unit/test_units.cpp
  unit/test_profiles.cpp
  unit/test_water_properties.cpp
  unit/test_permeability.cpp
  unit/test_flow.cpp
  unit/test_heat.cpp
  unit/test_extraction.cpp
  unit/test_artifacts.cpp
  unit/test_calibration.cpp
  integration/test_shots.cpp
  integration/test_invariants.cpp
  integration/test_convergence.cpp
  integration/test_sweeps.cpp
  integration/test_calibration_recovery.cpp
)
target_link_libraries(espressolab_tests
  PRIVATE catch2_amalgamated espressolab_core espressolab_artifacts espressolab_experiments
          espressolab_calibration espressolab_warnings)
target_compile_definitions(espressolab_tests
  PRIVATE ESPRESSOLAB_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets"
          ESPRESSOLAB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

include(CTest)
add_test(NAME espressolab_tests COMMAND espressolab_tests)
