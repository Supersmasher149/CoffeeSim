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
  unit/test_reference_io.cpp
  unit/test_calibration.cpp
  unit/test_calibration_io.cpp
  unit/test_cli_workflows.cpp
  unit/test_tui_forms.cpp
  integration/test_shots.cpp
  integration/test_invariants.cpp
  integration/test_convergence.cpp
  integration/test_axial_cells.cpp
  integration/test_cfd.cpp
  integration/test_cfd3d.cpp
  integration/test_sweeps.cpp
  integration/test_calibration_recovery.cpp
  integration/test_cancellation.cpp
)
# tp_json is header-only and lets the artifact tests assert the serialized
# contract by parsing it rather than by matching substrings. espressolab_cli_support
# (shared CLI workflow services and pure TUI form logic, issues #24/#25) has no
# FTXUI dependency, so linking it here does not pull a terminal UI dependency
# into the test binary (issue #30: "pure TUI tests are deterministic and do
# not require a real terminal").
target_link_libraries(espressolab_tests
  PRIVATE catch2_amalgamated espressolab_core espressolab_artifacts espressolab_references espressolab_experiments
          espressolab_calibration espressolab_cfd espressolab_warnings tp_json espressolab_cli_support)
target_compile_definitions(espressolab_tests
  PRIVATE ESPRESSOLAB_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets"
          ESPRESSOLAB_REFERENCE_DIR="${PROJECT_SOURCE_DIR}/espresso_real_world_refs"
          ESPRESSOLAB_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

include(CTest)
add_test(NAME espressolab_tests COMMAND espressolab_tests)

# Audit P1, issue #17: apps/espressolab_server/main.cpp is not linked into
# espressolab_tests (only the engine/library targets it uses are), so the
# server's request translation, error mapping, background job polling,
# cancellation, retention, and artifact export had no test target at all --
# not "untested", literally not registered anywhere ctest could find. These
# start the real built espressolab_server binary as a subprocess on an
# isolated local port and exercise the documented REST contract end to end;
# see each script's own docstring for what it covers. They need a real
# server binary, so they only register when ESPRESSOLAB_BUILD_SERVER gave us
# that target, and a Python 3 interpreter to run them with.
if(TARGET espressolab_server)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    foreach(smoke_script
        server_port_smoke
        cfd3d_error_smoke
        cfd3d_concurrency_smoke
        rest_integration_smoke
        measured_shots_smoke)
      add_test(
        NAME server_${smoke_script}
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/server/${smoke_script}.py
                $<TARGET_FILE:espressolab_server>)
      set_tests_properties(server_${smoke_script} PROPERTIES LABELS "integration;server")
    endforeach()
  endif()
endif()
