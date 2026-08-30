#include <catch_amalgamated.hpp>
#include <array>
#include <cmath>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/flavor.hpp"

using namespace espressolab;

namespace {

std::array<double, kSoluteClassCount> uniform_composition() {
    std::array<double, kSoluteClassCount> composition{};
    composition.fill(1.0 / static_cast<double>(kSoluteClassCount));
    return composition;
}

std::array<double, kSoluteClassCount> pure(SoluteClass klass) {
    std::array<double, kSoluteClassCount> composition{};
    composition[static_cast<std::size_t>(klass)] = 1.0;
    return composition;
}

std::size_t argmax(const std::array<double, kSensoryAxisCount>& values) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (values[i] > values[best]) best = i;
    }
    return best;
}

}  // namespace

TEST_CASE("strength gain is bounded and monotone in TDS", "[flavor][unit]") {
    REQUIRE(strength_gain(kTdsReferenceFraction) == Catch::Approx(1.0));
    REQUIRE(strength_gain(0.0) == Catch::Approx(kStrengthGainMin));
    // Bounded at both ends: a wildly concentrated shot must not run the axes off
    // the scale on concentration alone.
    REQUIRE(strength_gain(10.0) == Catch::Approx(kStrengthGainMax));
    double previous = -1.0;
    for (double tds = 0.01; tds < 0.25; tds += 0.005) {
        const double gain = strength_gain(tds);
        REQUIRE(gain >= previous);
        REQUIRE(gain >= kStrengthGainMin);
        REQUIRE(gain <= kStrengthGainMax);
        previous = gain;
    }
    // Sub-linear: doubling the concentration must not double the intensity.
    REQUIRE(strength_gain(2.0 * kTdsReferenceFraction) < 2.0 * strength_gain(kTdsReferenceFraction));
}

TEST_CASE("an even six-way cup reads neutral on every axis", "[flavor][unit]") {
    const auto intensity = axis_intensities(uniform_composition(), default_axis_weights(),
                                            kTdsReferenceFraction);
    // This is what the row-mean normalisation buys: a definite, shared centre.
    for (double value : intensity) REQUIRE(value == Catch::Approx(kNeutralIntensity));
}

TEST_CASE("each solute class drives the axis it is meant to", "[flavor][unit]") {
    const AxisWeightMatrix weights = default_axis_weights();
    const auto base = axis_intensities(uniform_composition(), weights, kTdsReferenceFraction);

    // Measured as enrichment from an even cup, not at a pure one: several axes
    // saturate at kIntensityMax when a single class holds the whole cup, and a
    // comparison between two clamped values says nothing. Enrichment is also
    // the question that actually matters -- when a shot gains acids, which axis
    // moves most.
    const auto axis_that_rises_most = [&](SoluteClass klass) {
        std::array<double, kSoluteClassCount> composition{};
        composition.fill((1.0 / static_cast<double>(kSoluteClassCount)) * 0.7);
        double remainder = 1.0;
        for (double share : composition) remainder -= share;
        composition[static_cast<std::size_t>(klass)] += remainder;

        const auto enriched = axis_intensities(composition, weights, kTdsReferenceFraction);
        std::array<double, kSensoryAxisCount> gain{};
        for (std::size_t a = 0; a < kSensoryAxisCount; ++a) gain[a] = enriched[a] - base[a];
        return argmax(gain);
    };

    REQUIRE(axis_that_rises_most(SoluteClass::acids) ==
            static_cast<std::size_t>(SensoryAxis::acidity));
    REQUIRE(axis_that_rises_most(SoluteClass::sugars) ==
            static_cast<std::size_t>(SensoryAxis::sweetness));
    REQUIRE(axis_that_rises_most(SoluteClass::maillard) ==
            static_cast<std::size_t>(SensoryAxis::chocolate));
    REQUIRE(axis_that_rises_most(SoluteClass::lipids) ==
            static_cast<std::size_t>(SensoryAxis::body));
    REQUIRE(axis_that_rises_most(SoluteClass::bitter) ==
            static_cast<std::size_t>(SensoryAxis::bitterness));
    REQUIRE(axis_that_rises_most(SoluteClass::polyphenols) ==
            static_cast<std::size_t>(SensoryAxis::astringency));
}

TEST_CASE("a cup dominated by one class saturates rather than running off scale",
          "[flavor][unit]") {
    // The clamp is load-bearing under row-mean normalisation: unlike the
    // row-max form, the ratio is not bounded above by 1.
    for (std::size_t c = 0; c < kSoluteClassCount; ++c) {
        const auto intensity =
            axis_intensities(pure(static_cast<SoluteClass>(c)), default_axis_weights(),
                             kTdsReferenceFraction);
        for (double value : intensity) {
            REQUIRE(value >= 0.0);
            REQUIRE(value <= kIntensityMax);
        }
    }
}

TEST_CASE("intensities stay on the scale for any composition", "[flavor][unit][property]") {
    const AxisWeightMatrix weights = default_axis_weights();
    // A deterministic sweep over the simplex corners and their pairwise mixes:
    // the extremes are where a normalisation bug would show.
    for (std::size_t a = 0; a < kSoluteClassCount; ++a) {
        for (std::size_t b = 0; b < kSoluteClassCount; ++b) {
            for (double split = 0.0; split <= 1.0; split += 0.1) {
                std::array<double, kSoluteClassCount> composition{};
                composition[a] += split;
                composition[b] += 1.0 - split;
                for (double tds : {0.001, 0.04, 0.09, 0.16, 0.40}) {
                    for (double value : axis_intensities(composition, weights, tds)) {
                        REQUIRE(std::isfinite(value));
                        REQUIRE(value >= 0.0);
                        REQUIRE(value <= kIntensityMax);
                    }
                }
            }
        }
    }
}

TEST_CASE("only a weight row's shape matters, not its scale", "[flavor][unit]") {
    const auto composition = uniform_composition();
    AxisWeightMatrix scaled = default_axis_weights();
    const std::size_t chocolate = static_cast<std::size_t>(SensoryAxis::chocolate);
    for (std::size_t c = 0; c < kSoluteClassCount; ++c) scaled[chocolate][c] *= 3.7;

    const auto base = axis_intensities(composition, default_axis_weights(), kTdsReferenceFraction);
    const auto tripled = axis_intensities(composition, scaled, kTdsReferenceFraction);
    // An author cannot buy a louder axis by writing bigger numbers.
    REQUIRE(tripled[chocolate] == Catch::Approx(base[chocolate]));
}

TEST_CASE("the match score measures distance from the bean's own target", "[flavor][unit]") {
    const BeanProfile bean = testing::hologram_bean();
    std::array<double, kSensoryAxisCount> on_target{};
    for (std::size_t a = 0; a < kSensoryAxisCount; ++a) on_target[a] = bean.target[a].intensity;

    SECTION("hitting the target exactly scores 100 and reads balanced") {
        const FlavorSummary summary =
            score_against_target(uniform_composition(), on_target, bean.target);
        REQUIRE(summary.match_score == Catch::Approx(100.0));
        REQUIRE(summary.rms_deviation == Catch::Approx(0.0));
        REQUIRE(summary.verdict == FlavorVerdict::balanced);
    }
    SECTION("the score falls as the shot drifts, and never below zero") {
        double previous = 101.0;
        for (double drift = 0.0; drift <= 8.0; drift += 0.5) {
            auto drifted = on_target;
            for (double& value : drifted) value = std::min(value + drift, kIntensityMax);
            const FlavorSummary summary =
                score_against_target(uniform_composition(), drifted, bean.target);
            REQUIRE(summary.match_score <= previous);
            REQUIRE(summary.match_score >= 0.0);
            REQUIRE(summary.match_score <= 100.0);
            previous = summary.match_score;
        }
        REQUIRE(previous == Catch::Approx(0.0));
    }
    SECTION("the axis furthest off is the one reported") {
        auto drifted = on_target;
        drifted[static_cast<std::size_t>(SensoryAxis::chocolate)] += 3.0;
        const FlavorSummary summary =
            score_against_target(uniform_composition(), drifted, bean.target);
        REQUIRE(summary.dominant_deviation_axis == SensoryAxis::chocolate);
        REQUIRE(summary.axes[static_cast<std::size_t>(SensoryAxis::chocolate)].deviation ==
                Catch::Approx(3.0));
    }
}

TEST_CASE("the verdict is relative to the bean, not to a universal ideal", "[flavor][unit]") {
    const BeanProfile bean = testing::hologram_bean();
    std::array<double, kSensoryAxisCount> axes{};
    for (std::size_t a = 0; a < kSensoryAxisCount; ++a) axes[a] = bean.target[a].intensity;

    const auto verdict_of = [&](double bright_shift) {
        auto shifted = axes;
        shifted[static_cast<std::size_t>(SensoryAxis::acidity)] += bright_shift;
        shifted[static_cast<std::size_t>(SensoryAxis::fruit)] += bright_shift;
        return score_against_target(uniform_composition(), shifted, bean.target).verdict;
    };

    REQUIRE(verdict_of(0.0) == FlavorVerdict::balanced);
    REQUIRE(verdict_of(kVerdictBandPoints) == FlavorVerdict::balanced);
    REQUIRE(verdict_of(2.0 * kVerdictBandPoints + 1.0) == FlavorVerdict::under_extracted_sour);
    REQUIRE(verdict_of(-(2.0 * kVerdictBandPoints + 1.0)) ==
            FlavorVerdict::over_extracted_bitter);

    SECTION("a bean that asks for high acidity is not called sour for delivering it") {
        // The same absolute intensities read differently against two targets.
        BeanProfile bright = bean;
        bright.target[static_cast<std::size_t>(SensoryAxis::acidity)].intensity = 9.0;
        bright.target[static_cast<std::size_t>(SensoryAxis::fruit)].intensity = 9.0;
        auto delivered = axes;
        delivered[static_cast<std::size_t>(SensoryAxis::acidity)] = 9.0;
        delivered[static_cast<std::size_t>(SensoryAxis::fruit)] = 9.0;

        REQUIRE(score_against_target(uniform_composition(), delivered, bright.target).verdict ==
                FlavorVerdict::balanced);
        REQUIRE(score_against_target(uniform_composition(), delivered, bean.target).verdict ==
                FlavorVerdict::under_extracted_sour);
    }
}
