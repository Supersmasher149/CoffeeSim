#pragma once
#include <filesystem>
#include <string>

#include "espressolab/result.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/types.hpp"

// Section 10: versioned, reloadable artifacts. This is the only component that
// knows about JSON, CSV and the filesystem.
namespace espressolab::artifact_io {

struct LoadError : std::runtime_error {
    LoadError(std::string code_, std::string path_, std::string message)
        : std::runtime_error(message), code(std::move(code_)), path(std::move(path_)) {}
    std::string code;
    std::string path;
};

Recipe load_recipe_json(const std::string& json_text);
Recipe load_recipe_file(const std::filesystem::path& file);
std::string dump_recipe_json(const Recipe& recipe, int indent = 2);

ModelCoefficients load_coefficients_json(const std::string& json_text);
ModelCoefficients load_coefficients_file(const std::filesystem::path& file);
std::string dump_coefficients_json(const ModelCoefficients& coeff, int indent = 2);

std::string dump_summary_json(const ShotResult& result, int indent = 2);
std::string dump_manifest_json(const ShotResult& result, int indent = 2);
std::string dump_result_json(const ShotResult& result, int indent = 2);
std::string dump_samples_csv(const ShotResult& result);

// Section 10.4: outputs/shots/<run-id>/{recipe,coefficients,summary,manifest}.json + samples.csv
void write_shot_artifacts(const std::filesystem::path& directory, const Recipe& recipe,
                          const ModelCoefficients& coeff, const ShotResult& result);

// Section 10.3: SHA-256 over canonicalised recipe + coefficients + solver
// configuration + ordered samples. A reproducibility signal, not a security
// boundary.
std::string sha256_hex(const std::string& bytes);
std::string recipe_hash(const Recipe& recipe);
std::string coefficient_hash(const ModelCoefficients& coeff);
std::string result_hash(const Recipe& recipe, const ModelCoefficients& coeff,
                        const SimulationConfig& config, const std::vector<ShotSample>& samples,
                        const std::vector<RegionSummary>& regions = {});

// Fills the identity fields of section 10.1 that need canonical JSON, which the
// solver itself cannot produce without breaking the dependency rule (3.4).
// The run id is derived from the result hash, so the same inputs land in the
// same output directory (FR-09).
void stamp_manifest(ShotResult& result, const Recipe& recipe, const ModelCoefficients& coeff,
                    const SimulationConfig& config);

}  // namespace espressolab::artifact_io
