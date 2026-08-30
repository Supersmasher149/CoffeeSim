#pragma once
#include <array>

#include "espressolab/bean.hpp"
#include "espressolab/flavor_result.hpp"

namespace espressolab {

// The sensory overlay's correlations. Pure functions over a cup composition and
// a bean's declarations; they hold no state and touch no solver variable, which
// is what lets the whole overlay be computed after the physics is done.
//
// These constants are deliberately NOT members of ModelCoefficients. Every
// member of that struct is hashed into coefficient_hash() and therefore into
// every result_hash, so adding one would rewrite the hash of every existing run
// for a code path those runs never take -- the same reasoning that keeps
// GrindDistribution's reference sigma out of the coefficient set.

// Mid of the 4-16% band the integration tests already treat as espresso-shaped.
inline constexpr double kTdsReferenceFraction = 0.09;
// Sub-linear: perceived intensity grows slower than concentration. An authored
// prior in the spirit of Stevens' law, not a fitted exponent.
inline constexpr double kStrengthExponent = 0.60;
inline constexpr double kStrengthGainMin = 0.30;
inline constexpr double kStrengthGainMax = 1.60;
inline constexpr double kIntensityMax = 10.0;
// An axis reads here when the cup is an even mix of all six classes. Above it the
// cup is enriched in the classes that axis listens to, below it depleted -- which
// is the only thing the composition can actually tell us.
inline constexpr double kNeutralIntensity = 5.0;
// A weighted RMS miss of this many intensity points scores zero.
inline constexpr double kScoreFullMissPoints = 4.00;
inline constexpr double kDefaultTolerancePoints = 1.50;
// How far the shot's acid/bitter balance may sit from the bean's declared
// balance before the verdict stops being "balanced".
inline constexpr double kVerdictBandPoints = 1.50;

// TDS -> a bounded, sub-linear perceived-strength multiplier. 1.0 at the
// reference TDS; a watery shot reads muted across every axis, a ristretto
// intense, without either changing the balance between axes.
[[nodiscard]] double strength_gain(double tds_fraction);

// Cup class fractions + strength -> 0-10 per axis.
//
// Each weight row is normalised by its own MEAN, not its maximum. Because
// `composition` is a probability vector, the raw response is a weighted average
// of the row's weights and so can only approach the row maximum if the entire
// cup is one solute class -- normalising by the maximum would therefore squash
// every real shot into the bottom half of the scale, and squash the bright axes
// hardest because their rows are the most peaked. Dividing by the mean puts 1.0
// at an even six-way mix, which kNeutralIntensity maps to the middle of the
// scale; above it the cup is enriched in what the axis listens to, below it
// depleted. Only the row's *shape* matters either way, so an author still cannot
// saturate an axis by writing large numbers.
//
// There is deliberately no separate extraction-yield term -- the shift from
// bright to bitter is already carried by the composition, and a second
// yield-driven knob would double-count the one mechanism the overlay exists to
// express (and smuggle in a "correct yield" judgement the project refuses).
[[nodiscard]] std::array<double, kSensoryAxisCount> axis_intensities(
    const std::array<double, kSoluteClassCount>& composition, const AxisWeightMatrix& weights,
    double tds_fraction);

// Intensities against the bean's declared target -> score, dominant miss, verdict.
[[nodiscard]] FlavorSummary score_against_target(
    const std::array<double, kSoluteClassCount>& composition,
    const std::array<double, kSensoryAxisCount>& intensity, const SensoryTarget& target);

}  // namespace espressolab
