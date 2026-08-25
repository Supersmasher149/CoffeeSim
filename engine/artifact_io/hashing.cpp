#include <chrono>
#include <format>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

#include "espressolab/artifact_io.hpp"
#include "espressolab/version.hpp"

namespace espressolab::artifact_io {
namespace {

// Section 10.3: canonicalise before hashing. nlohmann's object type is an
// ordered map by key, so dumping a parsed document gives the same bytes
// regardless of how the source file was formatted. Doubles are written at
// full precision so a hash cannot collide across visibly different inputs.
std::string canonicalise(const std::string& json_text) {
    return nlohmann::json::parse(json_text).dump();
}

std::string fixed(double value) {
    std::ostringstream out;
    out << std::setprecision(17) << value;
    return out.str();
}

}  // namespace

std::string recipe_hash(const Recipe& recipe) {
    return sha256_hex(canonicalise(dump_recipe_json(recipe, -1)));
}

std::string coefficient_hash(const ModelCoefficients& coeff) {
    return sha256_hex(canonicalise(dump_coefficients_json(coeff, -1)));
}

std::string result_hash(const Recipe& recipe, const ModelCoefficients& coeff,
                        const SimulationConfig& config, const std::vector<ShotSample>& samples,
                        const std::vector<RegionSummary>& regions) {
    std::ostringstream bytes;
    bytes << version::kSolver << '\n'
          << version::kResultSchema << '\n'
          << recipe_hash(recipe) << '\n'
          << coefficient_hash(coeff) << '\n'
          << fixed(config.dt_s) << '\n'
          << fixed(config.sample_interval_s) << '\n'
          << (config.strict_invariants ? 1 : 0) << '\n';

    // Ordered output samples, in solver order.
    for (const auto& s : samples) {
        bytes << fixed(s.time_s) << ',' << fixed(s.pressure_pa) << ','
              << fixed(s.inlet_temperature_k) << ',' << fixed(s.puck_temperature_k) << ','
               << fixed(s.flow_m3_s) << ',' << fixed(s.beverage_mass_kg) << ','
               << fixed(s.tds_fraction) << ',' << fixed(s.extraction_yield_fraction) << ','
               << fixed(s.saturation) << '\n';
    }
    for (const auto& region : regions) {
        bytes << fixed(region.area_fraction) << ',' << fixed(region.permeability_multiplier) << ','
              << fixed(region.beverage_mass_kg) << ',' << fixed(region.flow_fraction) << ','
              << fixed(region.tds_fraction) << ',' << fixed(region.extraction_yield_fraction) << '\n';
    }
    return sha256_hex(bytes.str());
}

void stamp_manifest(ShotResult& result, const Recipe& recipe, const ModelCoefficients& coeff,
                    const SimulationConfig& config) {
    RunManifest& m = result.manifest;
    m.recipe_hash = recipe_hash(recipe);
    m.coefficient_hash = coefficient_hash(coeff);
    m.result_hash = result_hash(recipe, coeff, config, result.samples, result.regions);
    m.run_id = "shot-" + m.result_hash.substr(0, 12);
    m.solver_version = std::string(version::kSolver);
    m.result_schema_version = std::string(version::kResultSchema);
    m.coefficient_id = coeff.id;
    m.coefficient_version = coeff.version;
    m.dt_s = config.dt_s;
    m.sample_interval_s = config.sample_interval_s;

    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    m.timestamp_utc = std::format("{:%Y-%m-%dT%H:%M:%SZ}", seconds);
}

}  // namespace espressolab::artifact_io
