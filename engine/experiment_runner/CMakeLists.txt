# experiment_runner owns sweeps, run schedules, aggregation and artifact
# naming. It renders no charts (3.1).
add_library(espressolab_experiments
  sweep.cpp
  sweep_io.cpp
)
add_library(espressolab::experiments ALIAS espressolab_experiments)
target_include_directories(espressolab_experiments PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(espressolab_experiments
  PUBLIC espressolab_core espressolab_artifacts
  PRIVATE tp_json espressolab_warnings)
