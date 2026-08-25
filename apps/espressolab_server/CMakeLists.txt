add_executable(espressolab_server main.cpp)
target_link_libraries(espressolab_server
  PRIVATE espressolab_core espressolab_artifacts espressolab_experiments tp_httplib tp_json
          espressolab_warnings)
