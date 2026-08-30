#include "espressolab/grinder_io.hpp"

#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "espressolab/units.hpp"

namespace espressolab::grinder_io {
namespace {

using nlohmann::json;

[[noreturn]] void fail(const std::string& code, const std::string& path,
                       const std::string& message) {
    ValidationResult result;
    result.add(code, message, path);
    throw InvalidInputError(result);
}

double number_or(const json& node, const char* key, double fallback, const std::string& path) {
    if (!node.contains(key)) return fallback;
    if (!node.at(key).is_number()) {
        fail("MISSING_FIELD", path + "." + key, std::string(key) + " must be a number");
    }
    return node.at(key).get<double>();
}

// Bin diameters round-trip through microns, and um -> m -> um is not an
// identity in binary floating point. Emitting at nanometre resolution makes a
// re-saved document byte-identical, the same rule recipe grind bins follow.
double canonical_micron_value(double metres) {
    constexpr double kScale = 1.0e6;
    return std::round(units::m_to_microns(metres) * kScale) / kScale;
}

json distribution_to_json(const GrindDistribution& distribution) {
    json bins = json::array();
    for (const GrindBin& bin : distribution.bins) {
        bins.push_back({{"diameter_um", canonical_micron_value(bin.diameter_m)},
                        {"mass_fraction", bin.mass_fraction}});
    }
    return json{{"bins", std::move(bins)}};
}

}  // namespace

GrinderSpec load_spec_json(const std::string& text) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::parse_error& e) {
        fail("MALFORMED_JSON", "grinder", e.what());
    }
    if (!root.is_object()) fail("MALFORMED_JSON", "grinder", "grinder spec must be an object");

    GrinderSpec spec;
    if (root.contains("name")) {
        if (!root.at("name").is_string()) {
            fail("MISSING_FIELD", "grinder.name", "name must be a string");
        }
        spec.name = root.at("name").get<std::string>();
    }
    spec.burr_gap_um = number_or(root, "burr_gap_um", spec.burr_gap_um, "grinder");
    spec.bean_diameter_um = number_or(root, "bean_diameter_um", spec.bean_diameter_um, "grinder");
    spec.selection_rate = number_or(root, "selection_rate", spec.selection_rate, "grinder");
    spec.selection_exponent =
        number_or(root, "selection_exponent", spec.selection_exponent, "grinder");
    spec.breakage_exponent =
        number_or(root, "breakage_exponent", spec.breakage_exponent, "grinder");
    spec.fines_yield = number_or(root, "fines_yield", spec.fines_yield, "grinder");
    spec.fines_diameter_um = number_or(root, "fines_diameter_um", spec.fines_diameter_um, "grinder");

    for (const char* key : {"passes", "bins"}) {
        if (!root.contains(key)) continue;
        if (!root.at(key).is_number_integer()) {
            fail("MISSING_FIELD", std::string("grinder.") + key,
                 std::string(key) + " must be an integer");
        }
        (std::string(key) == "passes" ? spec.passes : spec.bins) = root.at(key).get<int>();
    }
    return spec;
}

GrinderSpec load_spec_file(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) fail("FILE_NOT_FOUND", "grinder", "cannot open '" + file.string() + "'");
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return load_spec_json(buffer.str());
}

std::string dump_result_json(const GrinderSpec& spec, const GrinderResult& result, int indent) {
    json root;
    root["name"] = spec.name;
    root["spec"] = {{"burr_gap_um", spec.burr_gap_um},
                    {"bean_diameter_um", spec.bean_diameter_um},
                    {"passes", spec.passes},
                    {"selection_rate", spec.selection_rate},
                    {"selection_exponent", spec.selection_exponent},
                    {"breakage_exponent", spec.breakage_exponent},
                    {"fines_yield", spec.fines_yield},
                    {"fines_diameter_um", spec.fines_diameter_um},
                    {"bins", spec.bins}};
    root["distribution"] = distribution_to_json(result.distribution);
    root["derived"] = {{"sauter_mean_diameter_um", result.sauter_mean_diameter_um},
                       {"geometric_std_dev", result.geometric_std_dev},
                       {"cumulative_fines_fraction", result.cumulative_fines_fraction},
                       {"mass_balance_residual", result.mass_balance_residual}};
    // Stated in the artifact itself, not only in the docs, so a file that
    // outlives this repository still carries its own caveat.
    root["provenance"] = {
        {"model", "population balance: Austin selection, Broadbent-Callcott breakage, "
                  "cell-wall fines yield"},
        {"validated", false},
        {"limitations",
         json::array({"Coefficients are a plausible baseline, not fitted to any measured grind.",
                      "No particle size distribution in this project has been compared against a "
                      "measured one.",
                      "burr_gap_um is a physical length, not a grinder dial setting; nothing maps "
                      "a dial number onto it."})}};
    return root.dump(indent);
}

std::string dump_recipe_grind_json(const GrinderResult& result, int indent) {
    return distribution_to_json(result.distribution).dump(indent);
}

}  // namespace espressolab::grinder_io
