# Calibration is a separate, explicit workflow (section 11.3). Nothing in the
# solver depends on this target: a simulation never calibrates itself.
add_library(espressolab_calibration
  parameters.cpp
  loss.cpp
  fit.cpp
  calibration_io.cpp
)
add_library(espressolab::calibration ALIAS espressolab_calibration)
target_include_directories(espressolab_calibration PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(espressolab_calibration
  PUBLIC espressolab_core espressolab_artifacts
  PRIVATE tp_json espressolab_warnings)
