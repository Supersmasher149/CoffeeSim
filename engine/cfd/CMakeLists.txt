# Fidelity level 4: REV-scale two-phase porous-media CFD. Depends on the model
# library for its closures and on nothing above it (3.1).
add_library(espressolab_cfd cfd_solver.cpp)
add_library(espressolab::cfd ALIAS espressolab_cfd)
target_include_directories(espressolab_cfd PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(espressolab_cfd PUBLIC espressolab_core PRIVATE espressolab_warnings)
