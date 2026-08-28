# artifact_io owns versioned JSON/CSV loading, validation and result hashes
# for the standard Level 1-3 shot pipeline. It makes no physics decisions
# (3.1) and must not carry the separate Level 4b CFD3D solver as a hidden
# dependency of every standard artifact/simulation consumer (Audit F11,
# issue #16).
add_library(espressolab_artifacts
  sha256.cpp
  json_io.cpp
  hashing.cpp
)
add_library(espressolab::artifacts ALIAS espressolab_artifacts)
target_include_directories(espressolab_artifacts PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(espressolab_artifacts
  PUBLIC espressolab_core
  PRIVATE tp_json espressolab_warnings)

# cfd3d_io.cpp is CFD3D's own artifact I/O (Cfd3dCase/Cfd3dResult JSON,
# ELF3D-1 snapshot fields, manifests). It reuses espressolab_artifacts'
# recipe/coefficient JSON helpers and error type, but it is not part of the
# standard artifact contract, so it lives in its own target rather than in
# espressolab_artifacts above: linking espressolab_artifacts must not pull in
# espressolab_cfd3d for every standard simulation/artifact consumer (Audit
# F11, issue #16). Only CFD3D's own callers (CLI, server, tests) link this.
add_library(espressolab_cfd3d_artifacts
  cfd3d_io.cpp
)
add_library(espressolab::cfd3d_artifacts ALIAS espressolab_cfd3d_artifacts)
target_include_directories(espressolab_cfd3d_artifacts PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(espressolab_cfd3d_artifacts
  PUBLIC espressolab_core espressolab_cfd3d espressolab_artifacts
  PRIVATE tp_json espressolab_warnings)
