#include <algorithm>
#include <map>

#include "espressolab/calibration.hpp"

namespace espressolab::calibration {
namespace {

struct Accessor {
    double ModelCoefficients::*member;
    TunableParameter bounds;
};

// Only coefficients with a physical interpretation are fittable. Anything not
// in this table cannot be moved by a fit, which is the guardrail against
// "minimise the loss by turning every knob" (11.3).
const std::map<std::string, Accessor>& accessors() {
    static const std::map<std::string, Accessor> table{
        // Sets the overall resistance level; spans orders of magnitude, so it
        // is searched logarithmically.
        {"kozeny_constant",
         {&ModelCoefficients::kozeny_constant, {"kozeny_constant", 1.0e4, 1.0e9, true}}},
        {"extraction_rate_ref_s",
         {&ModelCoefficients::extraction_rate_ref_s, {"extraction_rate_ref_s", 0.01, 2.0, true}}},
        {"initial_porosity",
         {&ModelCoefficients::initial_porosity, {"initial_porosity", 0.25, 0.6, false}}},
        {"dry_permeability_multiplier",
         {&ModelCoefficients::dry_permeability_multiplier,
          {"dry_permeability_multiplier", 0.01, 1.0, false}}},
        {"pressure_compressibility",
         {&ModelCoefficients::pressure_compressibility,
          {"pressure_compressibility", 0.0, 0.5, false}}},
        {"maximum_compression",
         {&ModelCoefficients::maximum_compression, {"maximum_compression", 0.0, 0.6, false}}},
        {"extractable_solids_fraction",
         {&ModelCoefficients::extractable_solids_fraction,
          {"extractable_solids_fraction", 0.15, 0.45, false}}},
        {"grind_exponent",
         {&ModelCoefficients::grind_exponent, {"grind_exponent", 0.2, 3.0, false}}},
        {"activation_energy_j_mol",
         {&ModelCoefficients::activation_energy_j_mol,
          {"activation_energy_j_mol", 5000.0, 80000.0, false}}},
        {"flow_half_saturation_m3_s",
         {&ModelCoefficients::flow_half_saturation_m3_s,
          {"flow_half_saturation_m3_s", 1.0e-7, 1.0e-5, true}}},
        {"ambient_heat_loss_w_k",
         {&ModelCoefficients::ambient_heat_loss_w_k, {"ambient_heat_loss_w_k", 0.0, 5.0, false}}},
        {"coffee_heat_capacity_j_kg_k",
         {&ModelCoefficients::coffee_heat_capacity_j_kg_k,
          {"coffee_heat_capacity_j_kg_k", 1000.0, 2500.0, false}}},
    };
    return table;
}

}  // namespace

std::vector<std::string> tunable_parameter_names() {
    std::vector<std::string> names;
    names.reserve(accessors().size());
    for (const auto& [name, _] : accessors()) names.push_back(name);
    return names;
}

std::optional<TunableParameter> tunable_parameter(const std::string& name) {
    const auto it = accessors().find(name);
    if (it == accessors().end()) return std::nullopt;
    return it->second.bounds;
}

double read_parameter(const ModelCoefficients& coefficients, const std::string& name) {
    const auto it = accessors().find(name);
    if (it == accessors().end()) {
        ValidationResult result;
        result.add("UNKNOWN_PARAMETER_NAME", "no fittable coefficient named '" + name + "'", name);
        throw InvalidInputError(result);
    }
    return coefficients.*(it->second.member);
}

ModelCoefficients with_parameter(const ModelCoefficients& coefficients, const std::string& name,
                                 double value) {
    const auto it = accessors().find(name);
    if (it == accessors().end()) {
        ValidationResult result;
        result.add("UNKNOWN_PARAMETER_NAME", "no fittable coefficient named '" + name + "'", name);
        throw InvalidInputError(result);
    }
    ModelCoefficients copy = coefficients;
    copy.*(it->second.member) = value;
    return copy;
}

}  // namespace espressolab::calibration
