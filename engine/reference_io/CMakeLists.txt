add_library(espressolab_references
  reference_io.cpp
)
add_library(espressolab::references ALIAS espressolab_references)
target_include_directories(espressolab_references PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(espressolab_references PUBLIC tp_json PRIVATE espressolab_warnings)
