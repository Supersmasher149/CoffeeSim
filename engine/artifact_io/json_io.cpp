#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

#include "espressolab/artifact_io.hpp"
#include "espressolab/flavor.hpp"
#include "espressolab/units.hpp"
#include "espressolab/version.hpp"

// GCC's -Wdangling-reference flags every `const json& x = require_object(...)`
// call below as a possibly-dangling reference. It's a false positive: the
// referenced json_io.cpp:require_object legitimately returns a reference into
// its own reference parameter's subtree, whose lifetime already outlives the
// call; GCC's conservative heuristic can't see that through the call
// boundary. Clang has no equivalent warning, so this pair is GCC-only.
#if defined(__GNUC__) && !defined(__clang__)
#define ESPRESSOLAB_SUPPRESS_DANGLING_REF_BEGIN \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wdangling-reference\"")
#define ESPRESSOLAB_SUPPRESS_DANGLING_REF_END _Pragma("GCC diagnostic pop")
#else
#define ESPRESSOLAB_SUPPRESS_DANGLING_REF_BEGIN
#define ESPRESSOLAB_SUPPRESS_DANGLING_REF_END
#endif

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

void require_root_object(const json& node, const std::string& path) {
    if (!node.is_object()) fail("MISSING_FIELD", path, path + " must be a JSON object");
}

std::string require_string(const json& node, const char* key, const std::string& path) {
    if (!node.contains(key) || !node.at(key).is_string()) {
        fail("MISSING_FIELD", path + "." + key, std::string(key) + " must be a string");
    }
    return node.at(key).get<std::string>();
}

std::string optional_string(const json& node, const char* key, const std::string& path,
                            std::string fallback) {
    if (!node.contains(key) || node.at(key).is_null()) return fallback;
    if (!node.at(key).is_string()) {
        fail("MISSING_FIELD", path + "." + key, std::string(key) + " must be a string");
    }
    return node.at(key).get<std::string>();
}

double require_number(const json& node, const char* key, const std::string& path) {
    if (!node.contains(key) || !node.at(key).is_number() ||
        !std::isfinite(node.at(key).get<double>())) {
        fail("MISSING_FIELD", path + "." + key, std::string(key) + " must be a number");
    }
    return node.at(key).get<double>();
}

double required_number(const json& node, const char* key, const std::string& path) {
    if (!node.contains(key) || !node.at(key).is_number() ||
        !std::isfinite(node.at(key).get<double>())) {
        fail("MISSING_FIELD", path + "." + key, std::string(key) + " must be a finite number");
    }
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

// um -> m -> um is not an identity in binary floating point, so emitting the
// raw conversion makes load/dump/load/dump drift by an ulp each pass -- and
// since recipe_hash() is a digest of that document, simply re-saving a recipe
// would change its hash. Rounding to nanometre resolution (far finer than any
// real particle measurement, and finer than any grinder can resolve) makes the
// round trip an exact fixed point instead.
//
// Applied to grind bins only: the scalar particle_diameter_um has shipped
// without it, and re-rounding there would move hashes that already exist.
double canonical_micron_value(double metres) {
    constexpr double kScale = 1.0e6;  // six decimal places, i.e. 1 nm
    return std::round(units::m_to_microns(metres) * kScale) / kScale;
}

// A bean profile. The class and axis vocabularies are closed enums, so an
// unknown key is a load error naming the offending term rather than a silently
// ignored column -- the same stance parse_grind() takes on bin ordering.
BeanProfile parse_bean(const json& node, const std::string& path) {
    require_root_object(node, path);
    BeanProfile bean;
    bean.schema_version =
        optional_string(node, "schema_version", path, std::string(version::kBeanSchema));
    if (bean.schema_version != version::kBeanSchema) {
        fail("UNSUPPORTED_SCHEMA_VERSION", path + ".schema_version",
             "bean schema_version '" + bean.schema_version + "' is not supported (expected " +
                 std::string(version::kBeanSchema) + ")");
    }
    bean.id = require_string(node, "id", path);
    bean.version = optional_string(node, "version", path, "1.0.0");

    const std::array<double, kSoluteClassCount> default_rates = default_relative_rates();
    ESPRESSOLAB_SUPPRESS_DANGLING_REF_BEGIN
    const json& classes = require_object(node, "classes", path);
    ESPRESSOLAB_SUPPRESS_DANGLING_REF_END
    const std::string classes_path = path + ".classes";
    for (auto it = classes.begin(); it != classes.end(); ++it) {
        SoluteClass unused = SoluteClass::acids;
        if (!solute_class_from_string(it.key(), unused)) {
            fail("UNKNOWN_SOLUTE_CLASS", classes_path + "." + it.key(),
                 "'" + it.key() + "' is not a known solute class");
        }
    }
    for (std::size_t k = 0; k < kSoluteClassCount; ++k) {
        const char* name = to_string(static_cast<SoluteClass>(k));
        const std::string class_path = classes_path + "." + name;
        const json& entry = require_object(classes, name, classes_path);
        bean.classes[k].mass_fraction = required_number(entry, "mass_fraction", class_path);
        bean.classes[k].relative_rate =
            entry.contains("relative_rate")
                ? required_number(entry, "relative_rate", class_path)
                : default_rates[k];
    }

    // The weight matrix and the target both default to the shared model tables,
    // so a bean document only has to state what makes it that coffee.
    bean.axis_weights = default_axis_weights();
    if (node.contains("axis_weights")) {
        const json& weights = node.at("axis_weights");
        const std::string weights_path = path + ".axis_weights";
        require_root_object(weights, weights_path);
        for (auto it = weights.begin(); it != weights.end(); ++it) {
            SensoryAxis axis = SensoryAxis::fruit;
            if (!sensory_axis_from_string(it.key(), axis)) {
                fail("UNKNOWN_SENSORY_AXIS", weights_path + "." + it.key(),
                     "'" + it.key() + "' is not a known sensory axis");
            }
            const std::string row_path = weights_path + "." + it.key();
            require_root_object(it.value(), row_path);
            for (auto entry = it.value().begin(); entry != it.value().end(); ++entry) {
                SoluteClass klass = SoluteClass::acids;
                if (!solute_class_from_string(entry.key(), klass)) {
                    fail("UNKNOWN_SOLUTE_CLASS", row_path + "." + entry.key(),
                         "'" + entry.key() + "' is not a known solute class");
                }
                bean.axis_weights[static_cast<std::size_t>(axis)]
                                 [static_cast<std::size_t>(klass)] =
                    required_number(it.value(), entry.key().c_str(), row_path);
            }
        }
    }

    ESPRESSOLAB_SUPPRESS_DANGLING_REF_BEGIN
    const json& target = require_object(node, "target", path);
    ESPRESSOLAB_SUPPRESS_DANGLING_REF_END
    const std::string target_path = path + ".target";
    for (auto it = target.begin(); it != target.end(); ++it) {
        SensoryAxis unused = SensoryAxis::fruit;
        if (!sensory_axis_from_string(it.key(), unused)) {
            fail("UNKNOWN_SENSORY_AXIS", target_path + "." + it.key(),
                 "'" + it.key() + "' is not a known sensory axis");
        }
    }
    for (std::size_t a = 0; a < kSensoryAxisCount; ++a) {
        const char* name = to_string(static_cast<SensoryAxis>(a));
        const std::string axis_path = target_path + "." + name;
        const json& entry = require_object(target, name, target_path);
        bean.target[a].intensity = required_number(entry, "intensity", axis_path);
        bean.target[a].tolerance = entry.contains("tolerance")
                                       ? required_number(entry, "tolerance", axis_path)
                                       : kDefaultTolerancePoints;
        bean.target[a].weight =
            entry.contains("weight") ? required_number(entry, "weight", axis_path) : 1.0;
    }

    if (node.contains("description") && !node.at("description").is_null()) {
        const json& description = node.at("description");
        const std::string description_path = path + ".description";
        require_root_object(description, description_path);
        BeanDescription parsed;
        parsed.roaster = optional_string(description, "roaster", description_path, "");
        parsed.display_name = optional_string(description, "display_name", description_path, "");
        parsed.roast_level = optional_string(description, "roast_level", description_path, "");
        parsed.source = optional_string(description, "source", description_path, "");
        const auto string_list = [&](const char* key) {
            std::vector<std::string> out;
            if (!description.contains(key) || description.at(key).is_null()) return out;
            if (!description.at(key).is_array()) {
                fail("MISSING_FIELD", description_path + "." + key,
                     std::string(key) + " must be an array of strings");
            }
            for (const json& item : description.at(key)) {
                if (!item.is_string()) {
                    fail("MISSING_FIELD", description_path + "." + key,
                         std::string(key) + " must be an array of strings");
                }
                out.push_back(item.get<std::string>());
            }
            return out;
        };
        parsed.origins = string_list("origins");
        parsed.notes = string_list("notes");
        parsed.limitations = string_list("limitations");
        if (description.contains("suggested_ratio") &&
            !description.at("suggested_ratio").is_null()) {
            parsed.suggested_ratio = required_number(description, "suggested_ratio",
                                                     description_path);
        }
        bean.description = parsed;
    }
    return bean;
}

GrindDistribution parse_grind(const json& node, const std::string& path) {
    if (!node.is_object()) {
        fail("MISSING_FIELD", path, "grind must be an object with a bins array");
    }
    if (!node.contains("bins") || !node.at("bins").is_array()) {
        fail("MISSING_FIELD", path + ".bins", "grind.bins must be an array of size bins");
    }
    const json& bins = node.at("bins");

    GrindDistribution grind;
    grind.bins.reserve(bins.size());
    for (std::size_t i = 0; i < bins.size(); ++i) {
        const std::string bin_path = path + ".bins[" + std::to_string(i) + "]";
        if (!bins[i].is_object()) {
            fail("MISSING_FIELD", bin_path, "each grind bin must be an object");
        }
        // Ranges, ordering and the mass-fraction sum are GrindDistribution's
        // job, the same split the rest of this loader keeps: parse the shape
        // here, let validate() rule on the physics.
        grind.bins.push_back({units::microns_to_m(required_number(bins[i], "diameter_um", bin_path)),
                              required_number(bins[i], "mass_fraction", bin_path)});
    }
    return grind;
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
    require_root_object(root, "recipe");

    Recipe recipe;
    recipe.schema_version =
        optional_string(root, "schema_version", "recipe", std::string(version::kRecipeSchema));
    if (recipe.schema_version != version::kRecipeSchema) {
        // FR-01: unknown versions fail with a clear error.
        fail("UNSUPPORTED_SCHEMA_VERSION", "recipe.schema_version",
             "recipe schema_version '" + recipe.schema_version + "' is not supported (expected " +
                 std::string(version::kRecipeSchema) + ")");
    }
    recipe.name = optional_string(root, "name", "recipe", "unnamed");

    ESPRESSOLAB_SUPPRESS_DANGLING_REF_BEGIN
    const json& puck = require_object(root, "puck", "recipe");
    ESPRESSOLAB_SUPPRESS_DANGLING_REF_END
    recipe.dose_kg = units::grams_to_kg(require_number(puck, "dose_g", "recipe.puck"));
    recipe.basket_diameter_m =
        units::mm_to_m(require_number(puck, "basket_diameter_mm", "recipe.puck"));
    recipe.puck_depth_m = units::mm_to_m(require_number(puck, "depth_mm", "recipe.puck"));
    // Two mutually exclusive spellings of the same physical input: a single
    // representative diameter, or the distribution it stands in for. Supplying
    // both is rejected rather than silently resolved, so a reader never has to
    // guess which one the run actually used.
    const bool has_grind = puck.contains("grind");
    const bool has_scalar_grind =
        puck.contains("particle_diameter_um") || puck.contains("particle_spread_factor");
    if (has_grind && has_scalar_grind) {
        fail("CONFLICTING_FIELD", "recipe.puck.grind",
             "recipe.puck.grind and particle_diameter_um/particle_spread_factor are mutually "
             "exclusive; supply one or the other");
    }
    if (has_grind) {
        recipe.grind = parse_grind(puck.at("grind"), "recipe.puck.grind");
        // The scalars become derived values. Every downstream consumer -- the
        // solver, both CFD solvers, calibration -- keeps reading them and needs
        // no knowledge of the distribution.
        recipe.particle_diameter_m = recipe.grind->sauter_mean_diameter_m();
        recipe.particle_spread_factor = recipe.grind->equivalent_spread_factor();
    } else {
        recipe.particle_diameter_m =
            units::microns_to_m(require_number(puck, "particle_diameter_um", "recipe.puck"));
        recipe.particle_spread_factor =
            require_number(puck, "particle_spread_factor", "recipe.puck");
    }
    if (root.contains("bean") && !root.at("bean").is_null()) {
        recipe.bean = parse_bean(root.at("bean"), "recipe.bean");
    }
    if (root.contains("axial_cells")) {
        const json& cells = root.at("axial_cells");
        if (!cells.is_number_integer()) {
            fail("MISSING_FIELD", "recipe.axial_cells", "axial_cells must be an integer");
        }
        recipe.axial_cells = cells.get<int>();
    }
    if (root.contains("parallel_regions")) {
        const json& regions = root.at("parallel_regions");
        if (!regions.is_array() || regions.empty()) {
            fail("MISSING_FIELD", "recipe.parallel_regions",
                 "parallel_regions must be a non-empty array of region objects");
        }
        recipe.parallel_regions.clear();
        recipe.parallel_regions.reserve(regions.size());
        for (std::size_t i = 0; i < regions.size(); ++i) {
            const std::string path = "recipe.parallel_regions[" + std::to_string(i) + "]";
            if (!regions[i].is_object()) {
                fail("MISSING_FIELD", path, "each parallel region must be an object");
            }
            recipe.parallel_regions.push_back(
                {required_number(regions[i], "area_fraction", path),
                 required_number(regions[i], "permeability_multiplier", path)});
        }
    }

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

    ESPRESSOLAB_SUPPRESS_DANGLING_REF_BEGIN
    const json& stop = require_object(root, "stop", "recipe");
    ESPRESSOLAB_SUPPRESS_DANGLING_REF_END
    recipe.maximum_time_s = require_number(stop, "maximum_time_s", "recipe.stop");
    if (stop.contains("target_beverage_g") && !stop.at("target_beverage_g").is_null()) {
        recipe.target_beverage_mass_kg =
            units::grams_to_kg(require_number(stop, "target_beverage_g", "recipe.stop"));
    } else {
        recipe.target_beverage_mass_kg.reset();
    }
    return recipe;
}

Recipe load_recipe_file(const std::filesystem::path& file) {
    return load_recipe_json(read_file(file));
}

BeanProfile load_bean_json(const std::string& json_text) {
    return parse_bean(parse_or_throw(json_text, "bean"), "bean");
}

BeanProfile load_bean_file(const std::filesystem::path& file) {
    return load_bean_json(read_file(file));
}

json bean_to_json(const BeanProfile& bean) {
    json out;
    out["schema_version"] = bean.schema_version;
    out["id"] = bean.id;
    out["version"] = bean.version;
    json classes = json::object();
    for (std::size_t k = 0; k < kSoluteClassCount; ++k) {
        classes[to_string(static_cast<SoluteClass>(k))] = {
            {"mass_fraction", bean.classes[k].mass_fraction},
            {"relative_rate", bean.classes[k].relative_rate}};
    }
    out["classes"] = std::move(classes);
    json weights = json::object();
    for (std::size_t a = 0; a < kSensoryAxisCount; ++a) {
        json row = json::object();
        for (std::size_t c = 0; c < kSoluteClassCount; ++c) {
            row[to_string(static_cast<SoluteClass>(c))] = bean.axis_weights[a][c];
        }
        weights[to_string(static_cast<SensoryAxis>(a))] = std::move(row);
    }
    out["axis_weights"] = std::move(weights);
    json target = json::object();
    for (std::size_t a = 0; a < kSensoryAxisCount; ++a) {
        target[to_string(static_cast<SensoryAxis>(a))] = {
            {"intensity", bean.target[a].intensity},
            {"tolerance", bean.target[a].tolerance},
            {"weight", bean.target[a].weight}};
    }
    out["target"] = std::move(target);
    if (bean.description.has_value()) {
        const BeanDescription& description = *bean.description;
        json node;
        node["roaster"] = description.roaster;
        node["display_name"] = description.display_name;
        node["roast_level"] = description.roast_level;
        node["origins"] = description.origins;
        node["notes"] = description.notes;
        node["source"] = description.source;
        node["limitations"] = description.limitations;
        if (description.suggested_ratio.has_value()) {
            node["suggested_ratio"] = *description.suggested_ratio;
        } else {
            node["suggested_ratio"] = nullptr;
        }
        out["description"] = std::move(node);
    }
    return out;
}

std::string dump_recipe_json(const Recipe& recipe, int indent) {
    json root;
    root["schema_version"] = recipe.schema_version;
    root["name"] = recipe.name;
    root["puck"] = {{"dose_g", units::kg_to_grams(recipe.dose_kg)},
                    {"basket_diameter_mm", units::m_to_mm(recipe.basket_diameter_m)},
                    {"depth_mm", units::m_to_mm(recipe.puck_depth_m)}};
    if (recipe.grind.has_value()) {
        // The distribution is the authored input and the scalars are derived
        // from it, so the distribution is what gets serialized -- and therefore
        // what recipe_hash() is taken over. Emitting the derived scalars too
        // would hash the same fact twice and let a rounding change in the
        // derivation move the hash.
        json bins = json::array();
        for (const GrindBin& bin : recipe.grind->bins) {
            bins.push_back({{"diameter_um", canonical_micron_value(bin.diameter_m)},
                            {"mass_fraction", bin.mass_fraction}});
        }
        root["puck"]["grind"] = {{"bins", std::move(bins)}};
    } else {
        // Omitted entirely rather than written as null when there is no
        // distribution: recipe_hash() is a digest of this whole document, so an
        // unconditional key would change the hash of every recipe that predates
        // the PSD path. Same reason coefficient provenance is optional.
        root["puck"]["particle_diameter_um"] = units::m_to_microns(recipe.particle_diameter_m);
        root["puck"]["particle_spread_factor"] = recipe.particle_spread_factor;
    }
    if (recipe.bean.has_value()) {
        // Omitted entirely when absent, for exactly the reason the grind branch
        // above gives: recipe_hash() digests this whole document, so an
        // unconditional key would move the hash of every recipe written before
        // the sensory overlay existed.
        root["bean"] = bean_to_json(*recipe.bean);
    }
    root["axial_cells"] = recipe.axial_cells;
    root["parallel_regions"] = json::array();
    for (const ParallelRegion& region : recipe.parallel_regions) {
        root["parallel_regions"].push_back({{"area_fraction", region.area_fraction},
                                             {"permeability_multiplier",
                                              region.permeability_multiplier}});
    }
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
    require_root_object(root, "coefficients");
    ModelCoefficients c;
    c.id = require_string(root, "id", "coefficients");
    c.version = require_string(root, "version", "coefficients");
    ESPRESSOLAB_SUPPRESS_DANGLING_REF_BEGIN
    const json& v = require_object(root, "values", "coefficients");
    ESPRESSOLAB_SUPPRESS_DANGLING_REF_END
    c.initial_porosity = required_number(v, "initial_porosity", "coefficients.values");
    c.kozeny_constant = required_number(v, "kozeny_constant", "coefficients.values");
    c.dry_permeability_multiplier =
        required_number(v, "dry_permeability_multiplier", "coefficients.values");
    c.pressure_compressibility =
        required_number(v, "pressure_compressibility", "coefficients.values");
    c.maximum_compression = required_number(v, "maximum_compression", "coefficients.values");
    c.porosity_compression_factor =
        required_number(v, "porosity_compression_factor", "coefficients.values");
    c.minimum_porosity = required_number(v, "minimum_porosity", "coefficients.values");
    c.compression_reference_pa =
        required_number(v, "compression_reference_pa", "coefficients.values");
    c.coffee_heat_capacity_j_kg_k =
        required_number(v, "coffee_heat_capacity_j_kg_k", "coefficients.values");
    c.ambient_heat_loss_w_k =
        required_number(v, "ambient_heat_loss_w_k", "coefficients.values");
    c.ambient_temperature_k = units::celsius_to_kelvin(
        required_number(v, "ambient_temperature_c", "coefficients.values"));
    c.initial_puck_temperature_k = units::celsius_to_kelvin(
        required_number(v, "initial_puck_temperature_c", "coefficients.values"));
    c.extractable_solids_fraction =
        required_number(v, "extractable_solids_fraction", "coefficients.values");
    c.extraction_rate_ref_s =
        required_number(v, "extraction_rate_ref_s", "coefficients.values");
    c.activation_energy_j_mol =
        required_number(v, "activation_energy_j_mol", "coefficients.values");
    c.reference_temperature_k = units::celsius_to_kelvin(
        required_number(v, "reference_temperature_c", "coefficients.values"));
    c.grind_exponent = required_number(v, "grind_exponent", "coefficients.values");
    c.reference_particle_diameter_m = units::microns_to_m(
        required_number(v, "reference_particle_diameter_um", "coefficients.values"));
    c.flow_half_saturation_m3_s =
        required_number(v, "flow_half_saturation_m3_s", "coefficients.values");
    c.distribution_factor_floor =
        required_number(v, "distribution_factor_floor", "coefficients.values");
    c.maximum_flow_m3_s = required_number(v, "maximum_flow_m3_s", "coefficients.values");
    c.outlet_pressure_pa = required_number(v, "outlet_pressure_pa", "coefficients.values");

    // Issue #9, Audit F6: provenance was accepted by the schema but silently
    // dropped here, so calibration dataset/limitations metadata disappeared
    // the moment a coefficient document was reloaded. All fields are
    // individually optional, matching schemas/coefficients.schema.json
    // (only id/version/values are required at the document level).
    if (root.contains("provenance")) {
        ESPRESSOLAB_SUPPRESS_DANGLING_REF_BEGIN
        const json& p = require_object(root, "provenance", "coefficients");
        ESPRESSOLAB_SUPPRESS_DANGLING_REF_END
        CoefficientProvenance provenance;
        provenance.source = optional_string(p, "source", "coefficients.provenance", "");
        if (p.contains("dataset") && !p.at("dataset").is_null()) {
            if (!p.at("dataset").is_string()) {
                fail("MALFORMED_JSON", "coefficients.provenance.dataset", "dataset must be a string or null");
            }
            provenance.dataset = p.at("dataset").get<std::string>();
        }
        provenance.date = optional_string(p, "date", "coefficients.provenance", "");
        if (p.contains("limitations")) {
            if (!p.at("limitations").is_array()) {
                fail("MALFORMED_JSON", "coefficients.provenance.limitations",
                     "limitations must be an array of strings");
            }
            for (const json& item : p.at("limitations")) {
                if (!item.is_string()) {
                    fail("MALFORMED_JSON", "coefficients.provenance.limitations",
                         "limitations must contain strings");
                }
                provenance.limitations.push_back(item.get<std::string>());
            }
        }
        c.provenance = std::move(provenance);
    }
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
    // Issue #9, Audit F6: this used to write only id/version/values, so
    // provenance never survived being normalized into a run's
    // coefficients.json artifact even when the source document had it.
    if (c.provenance.has_value()) {
        const CoefficientProvenance& p = *c.provenance;
        root["provenance"] = {{"source", p.source},
                              {"dataset", p.dataset.has_value() ? json(*p.dataset) : json(nullptr)},
                              {"date", p.date},
                              {"limitations", p.limitations}};
    }
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

// The sensory overlay's headline block. Intensities are already the 0-10 scale
// the model reports, so unlike every other dumper here there is no unit
// conversion to do -- and deliberately none to invent.
json flavor_summary_to_json(const FlavorResult& flavor) {
    json out;
    out["bean_id"] = flavor.bean_id;
    out["bean_version"] = flavor.bean_version;
    out["flavor_model_version"] = flavor.flavor_model_version;
    out["match_score"] = flavor.summary.match_score;
    out["rms_deviation"] = flavor.summary.rms_deviation;
    out["verdict"] = to_string(flavor.summary.verdict);
    out["dominant_deviation_axis"] = to_string(flavor.summary.dominant_deviation_axis);
    out["class_clamp_count"] = flavor.summary.class_clamp_count;
    out["composition_residual_g"] = units::kg_to_grams(flavor.summary.composition_residual);
    json composition = json::object();
    for (std::size_t k = 0; k < kSoluteClassCount; ++k) {
        composition[to_string(static_cast<SoluteClass>(k))] =
            flavor.summary.composition[k] * 100.0;
    }
    out["composition_percent"] = std::move(composition);
    json axes = json::object();
    for (std::size_t a = 0; a < kSensoryAxisCount; ++a) {
        axes[to_string(static_cast<SensoryAxis>(a))] = {
            {"intensity", flavor.summary.axes[a].intensity},
            {"target", flavor.summary.axes[a].target},
            {"deviation", flavor.summary.axes[a].deviation}};
    }
    out["axes"] = std::move(axes);
    return out;
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
    root["regions"] = json::array();
    for (const RegionSummary& region : result.regions) {
        // Ordered from the screen side of the puck down to the basket.
        json cells = json::array();
        for (const AxialCellSummary& cell : region.cells) {
            cells.push_back({{"saturation", cell.saturation},
                             {"temperature_c", units::kelvin_to_celsius(cell.temperature_k)},
                             {"pore_tds_percent", cell.pore_tds_fraction * 100.0},
                             {"extraction_yield_percent", cell.extraction_yield_fraction * 100.0}});
        }
        root["regions"].push_back(
            {{"area_fraction", region.area_fraction},
             {"permeability_multiplier", region.permeability_multiplier},
             {"beverage_mass_g", units::kg_to_grams(region.beverage_mass_kg)},
             {"flow_fraction", region.flow_fraction},
             {"tds_percent", region.tds_fraction * 100.0},
             {"extraction_yield_percent", region.extraction_yield_fraction * 100.0},
             {"cells", std::move(cells)}});
    }
    if (result.flavor.has_value()) {
        root["flavor"] = flavor_summary_to_json(*result.flavor);
    }
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
    if (result.flavor.has_value()) {
        json series = json::array();
        for (const FlavorSample& sample : result.flavor->series) {
            json composition = json::object();
            for (std::size_t k = 0; k < kSoluteClassCount; ++k) {
                composition[to_string(static_cast<SoluteClass>(k))] = sample.composition[k] * 100.0;
            }
            json intensity = json::object();
            for (std::size_t a = 0; a < kSensoryAxisCount; ++a) {
                intensity[to_string(static_cast<SensoryAxis>(a))] = sample.intensity[a];
            }
            series.push_back({{"time_s", sample.time_s},
                              {"composition_percent", std::move(composition)},
                              {"intensity", std::move(intensity)}});
        }
        root["flavor"]["series"] = std::move(series);
    }
    return root.dump(indent);
}

std::string dump_flavor_series_csv(const ShotResult& result) {
    std::ostringstream out;
    out << "time_s";
    for (std::size_t k = 0; k < kSoluteClassCount; ++k) {
        out << ',' << to_string(static_cast<SoluteClass>(k)) << "_percent";
    }
    for (std::size_t a = 0; a < kSensoryAxisCount; ++a) {
        out << ',' << to_string(static_cast<SensoryAxis>(a));
    }
    out << '\n';
    if (!result.flavor.has_value()) return out.str();
    out << std::setprecision(10);
    for (const FlavorSample& sample : result.flavor->series) {
        out << sample.time_s;
        for (std::size_t k = 0; k < kSoluteClassCount; ++k) {
            out << ',' << sample.composition[k] * 100.0;
        }
        for (std::size_t a = 0; a < kSensoryAxisCount; ++a) {
            out << ',' << sample.intensity[a];
        }
        out << '\n';
    }
    return out.str();
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
    // A separate file rather than extra columns on samples.csv: that header is a
    // fixed contract with a stability test, and a beanless run must keep writing
    // exactly the five files it always has.
    if (result.flavor.has_value()) {
        write_file(directory / "flavor.csv", dump_flavor_series_csv(result));
    }
}

}  // namespace espressolab::artifact_io

#undef ESPRESSOLAB_SUPPRESS_DANGLING_REF_BEGIN
#undef ESPRESSOLAB_SUPPRESS_DANGLING_REF_END
