# model_library owns water properties, permeability, heat and extraction
# correlations. It holds no mutable run state (section 3.1).
add_library(espressolab_models
  water_properties.cpp
  puck.cpp
  extraction.cpp
)
add_library(espressolab::models ALIAS espressolab_models)
target_include_directories(espressolab_models PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(espressolab_models PUBLIC espressolab_core_types PRIVATE espressolab_warnings)
