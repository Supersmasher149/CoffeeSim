# espresso_core owns state variables, profiles, equations, stepping and
# termination. It knows nothing about HTTP, the browser or plotting (3.1/3.4).

# Domain vocabulary shared with model_library: split out so the correlations do
# not have to link the solver.
add_library(espressolab_core_types
  validation.cpp
  profile.cpp
  types.cpp
  grind.cpp
  result.cpp
)
target_include_directories(espressolab_core_types PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(espressolab_core_types PRIVATE espressolab_warnings)

add_library(espressolab_core simulator.cpp)
add_library(espressolab::core ALIAS espressolab_core)
target_include_directories(espressolab_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(espressolab_core
  PUBLIC espressolab_core_types espressolab_models
  PRIVATE espressolab_warnings)
