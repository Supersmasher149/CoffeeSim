#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "espressolab/cfd3d.hpp"

namespace espressolab::cfd3d_artifact_io {

struct Cfd3dCase {
    Recipe recipe;
    ModelCoefficients coefficients;
    Cfd3dConfig config;
};

struct Cfd3dRunManifest {
    std::string run_id;
    std::string case_schema_version;
    std::string result_schema_version;
    std::string field_format;
    std::string solver_version;
    std::string recipe_hash;
    std::string coefficient_hash;
    std::string result_hash;
    std::string timestamp_utc;
    double dt_s = 0.0;
    double sample_interval_s = 0.0;
    double snapshot_interval_s = 0.0;
    std::size_t snapshot_count = 0;
    std::uint64_t field_bytes = 0;
};

// Throws artifact_io::LoadError if `mesh` exceeds the documented dimension
// or cell-product limits (128 x 128 x 256, <=262144 cells -- the same bounds
// Cfd3dSolver::run() enforces). Callers that construct a dense
// Cfd3dField/Cfd3dMaterialField from a caller-supplied mesh (Audit F2,
// issue #5) must call this first, so an oversized mesh is rejected before
// allocation rather than after.
void validate_mesh_bounds(const Cfd3dMesh& mesh, const std::string& path);

Cfd3dCase load_case_json(const std::string& json_text);
Cfd3dCase load_case_file(const std::filesystem::path& file);
std::string dump_case_json(const Cfd3dCase& cfd3d_case, int indent = 2);

std::string result_hash(const Cfd3dCase& cfd3d_case, const Cfd3dResult& result,
                       const std::vector<Cfd3dSnapshot>& snapshots);
Cfd3dRunManifest make_manifest(const Cfd3dCase& cfd3d_case, const Cfd3dResult& result,
                               const std::vector<Cfd3dSnapshot>& snapshots);

std::string dump_manifest_json(const Cfd3dRunManifest& manifest, int indent = 2);
std::string dump_summary_json(const Cfd3dResult& result, int indent = 2);
std::string dump_samples_csv(const Cfd3dResult& result);

void write_artifacts(const std::filesystem::path& directory, const Cfd3dCase& cfd3d_case,
                     const Cfd3dResult& result,
                     const std::vector<Cfd3dSnapshot>& snapshots);

}  // namespace espressolab::cfd3d_artifact_io
