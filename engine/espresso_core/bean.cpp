#include "espressolab/bean.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "espressolab/version.hpp"

namespace espressolab {
namespace {

constexpr std::array<const char*, kSoluteClassCount> kSoluteClassNames = {
    "acids", "sugars", "maillard", "lipids", "bitter", "polyphenols"};

constexpr std::array<const char*, kSensoryAxisCount> kSensoryAxisNames = {
    "fruit", "acidity", "sweetness", "chocolate", "body", "bitterness", "astringency"};

}  // namespace

const char* to_string(SoluteClass value) {
    return kSoluteClassNames[static_cast<std::size_t>(value)];
}

const char* to_string(SensoryAxis value) {
    return kSensoryAxisNames[static_cast<std::size_t>(value)];
}

bool solute_class_from_string(const std::string& name, SoluteClass& out) {
    for (std::size_t i = 0; i < kSoluteClassCount; ++i) {
        if (name == kSoluteClassNames[i]) {
            out = static_cast<SoluteClass>(i);
            return true;
        }
    }
    return false;
}

bool sensory_axis_from_string(const std::string& name, SensoryAxis& out) {
    for (std::size_t i = 0; i < kSensoryAxisCount; ++i) {
        if (name == kSensoryAxisNames[i]) {
            out = static_cast<SensoryAxis>(i);
            return true;
        }
    }
    return false;
}

// Authored priors. The ordering is the well-established qualitative account of
// espresso extraction -- acids elute first, roast-degradation bitterness and
// tannins last -- but no value here is a measured rate constant.
//
// `bitter` is named for the sensation, not a chemical family: caffeine itself
// extracts fast, so calling this class "alkaloids" would imply a claim the
// number contradicts. It is a lumped late-eluting bitter class.
//
// `lipids` is the weakest entry of the six: an emulsified phase is stripped
// mechanically rather than dissolved, so a rate ratio is arguably the wrong
// functional form for it at all. Recorded as a limitation in
// assets/beans/README.md and docs/model.md rather than hidden here.
std::array<double, kSoluteClassCount> default_relative_rates() {
    return {2.60, 1.40, 1.00, 0.70, 0.45, 0.30};
}

AxisWeightMatrix default_axis_weights() {
    //            acids sugars maillard lipids bitter polyphenols
    return {{
        {{0.85, 0.35, 0.05, 0.00, 0.00, 0.10}},  // fruit
        {{1.00, 0.10, 0.00, 0.00, 0.00, 0.15}},  // acidity
        {{0.15, 1.00, 0.35, 0.10, 0.00, 0.00}},  // sweetness
        {{0.00, 0.30, 1.00, 0.20, 0.25, 0.05}},  // chocolate
        {{0.00, 0.20, 0.45, 1.00, 0.10, 0.15}},  // body
        {{0.05, 0.00, 0.25, 0.05, 1.00, 0.40}},  // bitterness
        {{0.10, 0.00, 0.05, 0.00, 0.35, 1.00}},  // astringency
    }};
}

ValidationResult BeanProfile::validate() const {
    ValidationResult result;

    if (schema_version != version::kBeanSchema) {
        result.add("UNSUPPORTED_SCHEMA_VERSION",
                   "bean schema_version '" + schema_version + "' is not supported (expected " +
                       std::string(version::kBeanSchema) + ")",
                   "recipe.bean.schema_version");
    }
    if (id.empty()) {
        result.add("MISSING_FIELD", "recipe.bean.id must not be empty", "recipe.bean.id");
    }

    // Mass fractions must partition the extractable solids exactly. The overlay
    // divides the mass the solver extracted; a set that does not sum to 1 would
    // silently rescale the composition and make the reported fractions a lie.
    double total_mass_fraction = 0.0;
    for (std::size_t i = 0; i < kSoluteClassCount; ++i) {
        const std::string base =
            std::string("recipe.bean.classes.") + kSoluteClassNames[i];
        const std::string mass_path = base + ".mass_fraction";
        const std::string rate_path = base + ".relative_rate";
        require_in_range(result, classes[i].mass_fraction, 0.0, 1.0, mass_path.c_str(), "");
        require_in_range(result, classes[i].relative_rate, 0.05, 10.0, rate_path.c_str(), "");
        total_mass_fraction += classes[i].mass_fraction;
    }
    if (std::abs(total_mass_fraction - 1.0) > 1.0e-9) {
        result.add("NONPHYSICAL_INPUT", "recipe.bean.classes mass fractions must sum to 1",
                   "recipe.bean.classes");
    }

    // Every row needs a positive maximum: the mapping normalises by it, and an
    // all-zero row would divide by zero rather than merely report a flat axis.
    for (std::size_t a = 0; a < kSensoryAxisCount; ++a) {
        double row_max = 0.0;
        for (std::size_t c = 0; c < kSoluteClassCount; ++c) {
            const std::string weight_path = std::string("recipe.bean.axis_weights.") +
                                            kSensoryAxisNames[a] + "." + kSoluteClassNames[c];
            require_in_range(result, axis_weights[a][c], 0.0, 10.0, weight_path.c_str(), "");
            row_max = std::max(row_max, axis_weights[a][c]);
        }
        if (!(row_max > 0.0)) {
            result.add("NONPHYSICAL_INPUT",
                       std::string("recipe.bean.axis_weights.") + kSensoryAxisNames[a] +
                           " must have at least one positive weight",
                       std::string("recipe.bean.axis_weights.") + kSensoryAxisNames[a]);
        }
    }

    double total_target_weight = 0.0;
    for (std::size_t a = 0; a < kSensoryAxisCount; ++a) {
        const std::string base = std::string("recipe.bean.target.") + kSensoryAxisNames[a];
        const std::string intensity_path = base + ".intensity";
        const std::string tolerance_path = base + ".tolerance";
        const std::string weight_path = base + ".weight";
        require_in_range(result, target[a].intensity, 0.0, 10.0, intensity_path.c_str(), "");
        require_in_range(result, target[a].tolerance, 0.1, 10.0, tolerance_path.c_str(), "");
        require_in_range(result, target[a].weight, 0.0, 10.0, weight_path.c_str(), "");
        total_target_weight += target[a].weight;
    }
    if (!(total_target_weight > 0.0)) {
        result.add("NONPHYSICAL_INPUT",
                   "recipe.bean.target must give a positive weight to at least one axis",
                   "recipe.bean.target");
    }

    return result;
}

}  // namespace espressolab
