#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

#include "espressolab/artifact_io.hpp"
#include "espressolab/units.hpp"
#include "espressolab/version.hpp"

namespace espressolab::artifact_io {
namespace {

using nlohmann::json;

[[noreturn]] void fail(const char* code, const std::string& path, const std::string& message) {
    throw LoadError(code, path, message);
}

const json& require_object(const json& node, const char* key, const std::string& path) {
    if (!node.contains(key) || !node.at(key).is_object()) {
        fail("MISSING_FIELD", path + "." + key, std::string(key) + " object is required");
    }
    return node.at(key);
}

double require_number(const json& node, const char* key, const std::string& path) {
    if (!node.contains(key) || !node.at(key).is_number()) {
        fail("MISSING_FIELD", path + "." + key, std::string(key) + " must be a number");
    }
    return node.at(key).get<double>();
}

double optional_number(const json& node, const char* key, double fallback) {
    if (!node.contains(key) || node.at(key).is_null()) return fallback;
    if (!node.at(key).is_number()) return fallback;
    return node.at(key).get<double>();
}

// Profiles arrive as [[t, v], ...] pairs (Appendix B).
PiecewiseLinearProfile parse_profile(const json& node, const std::string& path,
                                     double (*convert)(double)) {
    if (!node.is_array() || node.empty()) {
        fail("EMPTY_PROFILE", path, path + " must be a non-empty array of [time, value] pairs");
    }
    std::vector<ProfilePoint> points;
    points.reserve(node.size());
    for (std::size_t i = 0; i < node.size(); ++i) {
        const json& pair = node[i];
        const std::string point_path = path + "[" + std::to_string(i) + "]";
        if (!pair.is_array() || pair.size() != 2 || !pair[0].is_number() || !pair[1].is_number()) {
            fail("MALFORMED_PROFILE_POINT", point_path, "expected a [time_s, value] number pair");
        }
        points.push_back({pair[0].get<double>(), convert(pair[1].get<double>())});
    }
    return PiecewiseLinearProfile(std::move(points));
}

json profile_to_json(const PiecewiseLinearProfile& profile, double (*convert)(double)) {
    json out = json::array();
    for (const auto& point : profile.points()) {
        out.push_back(json::array({point.time_s, convert(point.value)}));
    }
    return out;
}

json parse_or_throw(const std::string& text, const std::string& path) {
    try {
        return json::parse(text);
    } catch (const json::parse_error& e) {
        fail("MALFORMED_JSON", path, e.what());
    }
}

std::string read_file(const std::filesystem::path& file) {
    std::ifstream stream(file);
    if (!stream) fail("FILE_NOT_FOUND", file.string(), "could not open " + file.string());
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void write_file(const std::filesystem::path& file, const std::string& contents) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) fail("WRITE_FAILED", file.string(), "could not write " + file.string());
    stream << contents;
}

json warnings_to_json(const std::vector<SimulationWarning>& warnings) {
    json out = json::array();
    for (const auto& w : warnings) {
        const char* severity = w.severity == WarningSeverity::hard   ? "hard"
                               : w.severity == WarningSeverity::soft ? "soft"
                                                                    : "info";
        out.push_back({{"code", w.code}, {"message", w.message}, {"time_s", w.time_s},
                       {"severity", severity}});
    }
    return out;
}

}  // namespace

Recipe load_recipe_json(const std::string& json_text) {
    const json root = parse_or_throw(json_text, "recipe");

    Recipe recipe;
    recipe.schema_version = root.value("schema_version", std::string(version::kRecipeSchema));
    if (recipe.schema_version != version::kRecipeSchema) {
        // FR-01: unknown versions fail with a clear error.
        fail("UNSUPPORTED_SCHEMA_VERSION", "recipe.schema_version",
             "recipe schema_version '" + recipe.schema_version + "' is not supported (expected " +
                 std::string(version::kRecipeSchema) + ")");
    }
    recipe.name = root.value("name", std::string("unnamed"));

    const json& puck = require_object(root, "puck", "recipe");
    recipe.dose_kg = units::grams_to_kg(require_number(puck, "dose_g", "recipe.puck"));
    recipe.basket_diameter_m =
        units::mm_to_m(require_number(puck, "basket_diameter_mm", "recipe.puck"));
    recipe.puck_depth_m = units::mm_to_m(require_number(puck, "depth_mm", "recipe.puck"));
    recipe.particle_diameter_m =
        units::microns_to_m(require_number(puck, "particle_diameter_um", "recipe.puck"));
    recipe.particle_spread_factor = require_number(puck, "particle_spread_factor", "recipe.puck");

    if (!root.contains("pressure_profile_bar")) {
        fail("MISSING_FIELD", "recipe.pressure_profile_bar", "pressure_profile_bar is required");
    }
    recipe.pressure_pa = parse_profile(root.at("pressure_profile_bar"), "recipe.pressure_profile_bar",
                                       units::bar_to_pa);
    if (!root.contains("temperature_profile_c")) {
        fail("MISSING_FIELD", "recipe.temperature_profile_c", "temperature_profile_c is required");
    }
    recipe.inlet_temperature_k = parse_profile(root.at("temperature_profile_c"),
                                               "recipe.temperature_profile_c",
                                               units::celsius_to_kelvin);

    const json& stop = require_object(root, "stop", "recipe");
    recipe.maximum_time_s = require_number(stop, "maximum_time_s", "recipe.stop");
    if (stop.contains("target_beverage_g") && !stop.at("target_beverage_g").is_null()) {
        recipe.target_beverage_mass_kg =
            units::grams_to_kg(stop.at("target_beverage_g").get<double>());
    } else {
        recipe.target_beverage_mass_kg.reset();
    }
    return recipe;
}

Recipe load_recipe_file(const std::filesystem::path& file) {
    return load_recipe_json(read_file(file));
}

std::string dump_recipe_json(const Recipe& recipe, int indent) {
    json root;
    root["schema_version"] = recipe.schema_version;
    root["name"] = recipe.name;
    root["puck"] = {{"dose_g", units::kg_to_grams(recipe.dose_kg)},
                    {"basket_diameter_mm", units::m_to_mm(recipe.basket_diameter_m)},
                    {"depth_mm", units::m_to_mm(recipe.puck_depth_m)},
                    {"particle_diameter_um", units::m_to_microns(recipe.particle_diameter_m)},
                    {"particle_spread_factor", recipe.particle_spread_factor}};
    root["pressure_profile_bar"] = profile_to_json(recipe.pressure_pa, units::pa_to_bar);
    root["temperature_profile_c"] =
        profile_to_json(recipe.inlet_temperature_k, units::kelvin_to_celsius);
    root["stop"]["maximum_time_s"] = recipe.maximum_time_s;
    if (recipe.target_beverage_mass_kg.has_value()) {
        root["stop"]["target_beverage_g"] = units::kg_to_grams(*recipe.target_beverage_mass_kg);
    } else {
        root["stop"]["target_beverage_g"] = nullptr;
    }
    return root.dump(indent);
}

ModelCoefficients load_coefficients_json(const std::string& json_text) {
    const json root = parse_or_throw(json_text, "coefficients");
    ModelCoefficients c;
    c.id = root.value("id", std::string("default"));
    c.version = root.value("version", std::string("1.0.0"));

    const json& v = root.contains("values") && root.at("values").is_object() ? root.at("values")
                                                                            : root;
    c.initial_porosity = optional_number(v, "initial_porosity", c.initial_porosity);
    c.kozeny_constant = optional_number(v, "kozeny_constant", c.kozeny_constant);
    c.dry_permeability_multiplier =
        optional_number(v, "dry_permeability_multiplier", c.dry_permeability_multiplier);
    c.pressure_compressibility =
        optional_number(v, "pressure_compressibility", c.pressure_compressibility);
    c.maximum_compression = optional_number(v, "maximum_compression", c.maximum_compression);
    c.porosity_compression_factor =
        optional_number(v, "porosity_compression_factor", c.porosity_compression_factor);
    c.minimum_porosity = optional_number(v, "minimum_porosity", c.minimum_porosity);
    c.compression_reference_pa =
        optional_number(v, "compression_reference_pa", c.compression_reference_pa);
    c.coffee_heat_capacity_j_kg_k =
        optional_number(v, "coffee_heat_capacity_j_kg_k", c.coffee_heat_capacity_j_kg_k);
    c.ambient_heat_loss_w_k = optional_number(v, "ambient_heat_loss_w_k", c.ambient_heat_loss_w_k);
    if (v.contains("ambient_temperature_c") && v.at("ambient_temperature_c").is_number()) {
        c.ambient_temperature_k =
            units::celsius_to_kelvin(v.at("ambient_temperature_c").get<double>());
    }
    if (v.contains("initial_puck_temperature_c") &&
        v.at("initial_puck_temperature_c").is_number()) {
        c.initial_puck_temperature_k =
            units::celsius_to_kelvin(v.at("initial_puck_temperature_c").get<double>());
    }
    c.extractable_solids_fraction =
        optional_number(v, "extractable_solids_fraction", c.extractable_solids_fraction);
    c.extraction_rate_ref_s = optional_number(v, "extraction_rate_ref_s", c.extraction_rate_ref_s);
    c.activation_energy_j_mol =
        optional_number(v, "activation_energy_j_mol", c.activation_energy_j_mol);
    if (v.contains("reference_temperature_c") && v.at("reference_temperature_c").is_number()) {
        c.reference_temperature_k =
            units::celsius_to_kelvin(v.at("reference_temperature_c").get<double>());
    }
    c.grind_exponent = optional_number(v, "grind_exponent", c.grind_exponent);
    if (v.contains("reference_particle_diameter_um") &&
        v.at("reference_particle_diameter_um").is_number()) {
        c.reference_particle_diameter_m =
            units::microns_to_m(v.at("reference_particle_diameter_um").get<double>());
    }
    c.flow_half_saturation_m3_s =
        optional_number(v, "flow_half_saturation_m3_s", c.flow_half_saturation_m3_s);
    c.distribution_factor_floor =
        optional_number(v, "distribution_factor_floor", c.distribution_factor_floor);
    c.maximum_flow_m3_s = optional_number(v, "maximum_flow_m3_s", c.maximum_flow_m3_s);
    c.outlet_pressure_pa = optional_number(v, "outlet_pressure_pa", c.outlet_pressure_pa);
    return c;
}

ModelCoefficients load_coefficients_file(const std::filesystem::path& file) {
    return load_coefficients_json(read_file(file));
}

std::string dump_coefficients_json(const ModelCoefficients& c, int indent) {
    json root;
    root["id"] = c.id;
    root["version"] = c.version;
    root["values"] = {
        {"initial_porosity", c.initial_porosity},
        {"kozeny_constant", c.kozeny_constant},
        {"dry_permeability_multiplier", c.dry_permeability_multiplier},
        {"pressure_compressibility", c.pressure_compressibility},
        {"maximum_compression", c.maximum_compression},
        {"porosity_compression_factor", c.porosity_compression_factor},
        {"minimum_porosity", c.minimum_porosity},
        {"compression_reference_pa", c.compression_reference_pa},
        {"coffee_heat_capacity_j_kg_k", c.coffee_heat_capacity_j_kg_k},
        {"ambient_heat_loss_w_k", c.ambient_heat_loss_w_k},
        {"ambient_temperature_c", units::kelvin_to_celsius(c.ambient_temperature_k)},
        {"initial_puck_temperature_c", units::kelvin_to_celsius(c.initial_puck_temperature_k)},
        {"extractable_solids_fraction", c.extractable_solids_fraction},
        {"extraction_rate_ref_s", c.extraction_rate_ref_s},
        {"activation_energy_j_mol", c.activation_energy_j_mol},
        {"reference_temperature_c", units::kelvin_to_celsius(c.reference_temperature_k)},
        {"grind_exponent", c.grind_exponent},
        {"reference_particle_diameter_um", units::m_to_microns(c.reference_particle_diameter_m)},
        {"flow_half_saturation_m3_s", c.flow_half_saturation_m3_s},
        {"distribution_factor_floor", c.distribution_factor_floor},
        {"maximum_flow_m3_s", c.maximum_flow_m3_s},
        {"outlet_pressure_pa", c.outlet_pressure_pa}};
    return root.dump(indent);
}

std::string dump_manifest_json(const ShotResult& result, int indent) {
    const RunManifest& m = result.manifest;
    json root = {{"run_id", m.run_id},
                 {"result_schema_version", m.result_schema_version},
                 {"solver_version", m.solver_version},
                 {"recipe_hash", m.recipe_hash},
                 {"coefficient_hash", m.coefficient_hash},
                 {"result_hash", m.result_hash},
                 {"coefficient_id", m.coefficient_id},
                 {"coefficient_version", m.coefficient_version},
                 {"timestamp_utc", m.timestamp_utc},
                 {"dt_s", m.dt_s},
                 {"sample_interval_s", m.sample_interval_s}};
    return root.dump(indent);
}

std::string dump_summary_json(const ShotResult& result, int indent) {
    const ShotSummary& s = result.summary;
    const ShotDiagnostics& d = result.diagnostics;
    json root = {
        {"termination", to_string(s.termination)},
        {"elapsed_time_s", s.elapsed_time_s},
        {"target_mass_reached", s.target_mass_reached},
        {"beverage_mass_g", units::kg_to_grams(s.beverage_mass_kg)},
        {"average_flow_ml_s", units::m3_s_to_ml_s(s.average_flow_m3_s)},
        {"peak_flow_ml_s", units::m3_s_to_ml_s(s.peak_flow_m3_s)},
        {"tds_percent", s.tds_fraction * 100.0},
        {"extraction_yield_percent", s.extraction_yield_fraction * 100.0},
        {"brew_ratio", s.brew_ratio},
        {"warning_count", s.warning_count},
        {"diagnostics",
         {{"water_mass_residual_g", units::kg_to_grams(d.water_mass_residual_kg)},
          {"solids_mass_residual_g", units::kg_to_grams(d.solids_mass_residual_kg)},
          {"clamp_count", d.clamp_count},
          {"step_count", d.step_count},
          {"min_permeability_m2", d.min_permeability_m2},
          {"max_flow_ml_s", units::m3_s_to_ml_s(d.max_flow_m3_s)},
          {"min_puck_temperature_c", units::kelvin_to_celsius(d.min_puck_temperature_k)},
          {"max_puck_temperature_c", units::kelvin_to_celsius(d.max_puck_temperature_k)}}},
        {"warnings", warnings_to_json(result.warnings)}};
    return root.dump(indent);
}

std::string dump_result_json(const ShotResult& result, int indent) {
    json root = json::parse(dump_summary_json(result, -1));
    root["manifest"] = json::parse(dump_manifest_json(result, -1));

    json samples = json::array();
    for (const auto& s : result.samples) {
        samples.push_back({{"time_s", s.time_s},
                           {"pressure_bar", units::pa_to_bar(s.pressure_pa)},
                           {"inlet_temperature_c", units::kelvin_to_celsius(s.inlet_temperature_k)},
                           {"puck_temperature_c", units::kelvin_to_celsius(s.puck_temperature_k)},
                           {"flow_ml_s", units::m3_s_to_ml_s(s.flow_m3_s)},
                           {"beverage_mass_g", units::kg_to_grams(s.beverage_mass_kg)},
                           {"tds_percent", s.tds_fraction * 100.0},
                           {"extraction_yield_percent", s.extraction_yield_fraction * 100.0},
                           {"saturation", s.saturation}});
    }
    root["samples"] = std::move(samples);
    return root.dump(indent);
}

// Stable column order so the CSV opens the same way every run (FR-07).
std::string dump_samples_csv(const ShotResult& result) {
    std::ostringstream out;
    out << "time_s,pressure_bar,inlet_temperature_c,puck_temperature_c,flow_ml_s,"
           "beverage_mass_g,tds_percent,extraction_yield_percent,saturation\n";
    out << std::setprecision(10);
    for (const auto& s : result.samples) {
        out << s.time_s << ',' << units::pa_to_bar(s.pressure_pa) << ','
            << units::kelvin_to_celsius(s.inlet_temperature_k) << ','
            << units::kelvin_to_celsius(s.puck_temperature_k) << ','
            << units::m3_s_to_ml_s(s.flow_m3_s) << ',' << units::kg_to_grams(s.beverage_mass_kg)
            << ',' << s.tds_fraction * 100.0 << ',' << s.extraction_yield_fraction * 100.0 << ','
            << s.saturation << '\n';
    }
    return out.str();
}

void write_shot_artifacts(const std::filesystem::path& directory, const Recipe& recipe,
                          const ModelCoefficients& coeff, const ShotResult& result) {
    std::filesystem::create_directories(directory);
    write_file(directory / "recipe.json", dump_recipe_json(recipe));
    write_file(directory / "coefficients.json", dump_coefficients_json(coeff));
    write_file(directory / "summary.json", dump_summary_json(result));
    write_file(directory / "manifest.json", dump_manifest_json(result));
    write_file(directory / "samples.csv", dump_samples_csv(result));
}

}  // namespace espressolab::artifact_io
