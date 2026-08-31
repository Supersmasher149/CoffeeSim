#include "espressolab/flavor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace espressolab {
namespace {

// The acid/bitter balance of a 0-10 axis vector. Positive means the bright side
// leads. Used only as a difference against the bean's own target, never as an
// absolute judgement.
double balance_of(const std::array<double, kSensoryAxisCount>& axes) {
    const double bright = (axes[static_cast<std::size_t>(SensoryAxis::acidity)] +
                           axes[static_cast<std::size_t>(SensoryAxis::fruit)]) /
                          2.0;
    const double heavy = (axes[static_cast<std::size_t>(SensoryAxis::bitterness)] +
                          axes[static_cast<std::size_t>(SensoryAxis::astringency)]) /
                         2.0;
    return bright - heavy;
}

}  // namespace

double strength_gain(double tds_fraction) {
    if (!(tds_fraction > 0.0)) return kStrengthGainMin;
    const double ratio = tds_fraction / kTdsReferenceFraction;
    return std::clamp(std::pow(ratio, kStrengthExponent), kStrengthGainMin, kStrengthGainMax);
}

std::array<double, kSensoryAxisCount> axis_intensities(
    const std::array<double, kSoluteClassCount>& composition, const AxisWeightMatrix& weights,
    double tds_fraction) {
    std::array<double, kSensoryAxisCount> intensity{};
    const double gain = strength_gain(tds_fraction);
    for (std::size_t a = 0; a < kSensoryAxisCount; ++a) {
        double response = 0.0;
        double row_total = 0.0;
        for (std::size_t c = 0; c < kSoluteClassCount; ++c) {
            response += weights[a][c] * composition[c];
            row_total += weights[a][c];
        }
        const double row_mean = row_total / static_cast<double>(kSoluteClassCount);
        // BeanProfile::validate() rejects an all-zero row, so this guard only
        // covers a hand-built profile that skipped validation.
        const double relative = row_mean > 0.0 ? response / row_mean : 0.0;
        // Unlike the row-max form, this ratio is not bounded above by 1, so the
        // clamp below does real work on a cup dominated by one class.
        intensity[a] = std::clamp(kNeutralIntensity * relative * gain, 0.0, kIntensityMax);
    }
    return intensity;
}

FlavorSummary score_against_target(const std::array<double, kSoluteClassCount>& composition,
                                   const std::array<double, kSensoryAxisCount>& intensity,
                                   const SensoryTarget& target) {
    FlavorSummary summary;
    summary.composition = composition;

    std::array<double, kSensoryAxisCount> target_intensity{};
    double weighted_square_sum = 0.0;
    double weight_sum = 0.0;
    double dominant_weighted_miss = -1.0;

    for (std::size_t a = 0; a < kSensoryAxisCount; ++a) {
        const double deviation = intensity[a] - target[a].intensity;
        summary.axes[a].intensity = intensity[a];
        summary.axes[a].target = target[a].intensity;
        summary.axes[a].deviation = deviation;
        target_intensity[a] = target[a].intensity;

        // A generous tolerance discounts the miss; a tight one amplifies it, so
        // a bean can say which axes it actually cares about being right on.
        const double scaled = deviation / (target[a].tolerance / kDefaultTolerancePoints);
        weighted_square_sum += target[a].weight * scaled * scaled;
        weight_sum += target[a].weight;

        const double weighted_miss = target[a].weight * std::abs(deviation);
        if (weighted_miss > dominant_weighted_miss) {
            dominant_weighted_miss = weighted_miss;
            summary.dominant_deviation_axis = static_cast<SensoryAxis>(a);
        }
    }

    summary.rms_deviation = weight_sum > 0.0 ? std::sqrt(weighted_square_sum / weight_sum) : 0.0;
    summary.match_score =
        100.0 * std::max(0.0, 1.0 - summary.rms_deviation / kScoreFullMissPoints);

    // Bean-relative: "sour for what this coffee asks for", not against a
    // universal ideal the project has consistently declined to encode.
    const double balance_offset = balance_of(intensity) - balance_of(target_intensity);
    if (balance_offset > kVerdictBandPoints) {
        summary.verdict = FlavorVerdict::under_extracted_sour;
    } else if (balance_offset < -kVerdictBandPoints) {
        summary.verdict = FlavorVerdict::over_extracted_bitter;
    } else {
        summary.verdict = FlavorVerdict::balanced;
    }
    return summary;
}

}  // namespace espressolab
