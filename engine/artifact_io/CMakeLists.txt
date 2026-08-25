# artifact_io owns versioned JSON/CSV loading, validation and result hashes.
# It makes no physics decisions (3.1).
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
