add_executable(espressolab_cli main.cpp)
target_link_libraries(espressolab_cli
  PRIVATE espressolab_core espressolab_artifacts espressolab_experiments espressolab_calibration
          espressolab_cfd espressolab_cfd3d espressolab_warnings tp_json)
