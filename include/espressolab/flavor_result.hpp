#pragma once
#include <array>
#include <string>
#include <vector>

#include "espressolab/bean.hpp"

namespace espressolab {

// Where the shot landed relative to the bean's own declared target, not to any
// universal ideal. A coffee whose roaster asks for high acidity is not
// "under-extracted" for delivering it.
enum class FlavorVerdict { under_extracted_sour, balanced, over_extracted_bitter };
const char* to_string(FlavorVerdict value);

// One point of the flavour time series. Carried in a vector PARALLEL to
// ShotResult::samples rather than as fields on ShotSample, so the nine hashed
// sample fields and dump_samples_csv()'s fixed column order stay exactly as they
// are for every run, bean or no bean.
struct FlavorSample {
    double time_s = 0.0;
    std::array<double, kSoluteClassCount> composition{};  // cup fractions, sum 1
    std::array<double, kSensoryAxisCount> intensity{};    // 0-10
};

struct FlavorAxisScore {
    double intensity = 0.0;  // 0-10, what this shot delivered
    double target = 0.0;     // 0-10, what the bean declares
    double deviation = 0.0;  // intensity - target
};

struct FlavorSummary {
    std::array<double, kSoluteClassCount> composition{};
    std::array<FlavorAxisScore, kSensoryAxisCount> axes{};
    double match_score = 0.0;    // 0-100 against the bean's target
    double rms_deviation = 0.0;  // weighted, in intensity points
    SensoryAxis dominant_deviation_axis = SensoryAxis::fruit;
    FlavorVerdict verdict = FlavorVerdict::balanced;
    // FR-08: clamps are never silent. class_clamp_count counts class pools that
    // hit their own floor; composition_residual closes the class masses against
    // dissolved_solids_in_cup_kg, the overlay's own mass balance.
    int class_clamp_count = 0;
    double composition_residual = 0.0;
};

struct FlavorResult {
    std::string bean_id;
    std::string bean_version;
    std::string flavor_model_version;
    FlavorSummary summary;
    std::vector<FlavorSample> series;
};

}  // namespace espressolab
