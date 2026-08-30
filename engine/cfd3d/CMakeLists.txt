# Isolated Level 4b Cartesian REV-scale solver. It shares domain and model
# closures with the existing core but is not part of the default request path.
add_library(espressolab_cfd3d
  cfd3d_geometry.cpp
  cfd3d_pressure.cpp
  cfd3d_solver.cpp
)
add_library(espressolab::cfd3d ALIAS espressolab_cfd3d)
target_include_directories(espressolab_cfd3d PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(espressolab_cfd3d PUBLIC espressolab_core PRIVATE espressolab_warnings)
