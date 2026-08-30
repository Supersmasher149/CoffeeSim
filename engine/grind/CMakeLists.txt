# A comminution model: burr geometry in, particle size distribution out.
# Deliberately outside the shot pipeline, in the same position as the CFD
# solvers (3.1) -- it depends only on the core domain types and cannot
# influence a shot's artifacts or result hash.
add_library(espressolab_grind grinder.cpp grinder_io.cpp)
add_library(espressolab::grind ALIAS espressolab_grind)
target_include_directories(espressolab_grind PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(espressolab_grind PUBLIC espressolab_core_types PRIVATE espressolab_warnings tp_json)
