#include <catch_amalgamated.hpp>
#include <cmath>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/calibration.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;
using namespace espressolab::calibration;

namespace {

// Builds a measured shot from the model's own output, which is how the
// machinery is exercised without real data.
MeasuredShot synthesize(const Recipe& recipe, const ModelCoefficients& coefficients,
                        const std::string& id, double sample_interval_s = 0.2) {
    const ShotResult result = Simulator().run(recipe, coefficients);

    MeasuredShot shot;
    shot.id = id;
    shot.source_stem = id;
    shot.recipe = recipe;
    shot.synthetic = true;

    double next = 0.0;
    for (const auto& sample : result.samples) {
        if (sample.time_s + 1.0e-9 < next) continue;
        next += sample_interval_s;
        shot.series.push_back({sample.time_s, units::kg_to_grams(sample.beverage_mass_kg), {}});
    }
    shot.final_beverage_mass_g = units::kg_to_grams(result.summary.beverage_mass_kg);
    shot.final_shot_time_s = result.summary.elapsed_time_s;
    shot.final_tds_percent = result.summary.tds_fraction * 100.0;
    return shot;
}

}  // namespace

TEST_CASE("beverage mass interpolates between solver samples", "[calibration]") {
    std::vector<ShotSample> samples(3);
    samples[0].time_s = 0.0;
    samples[0].beverage_mass_kg = 0.0;
    samples[1].time_s = 10.0;
    samples[1].beverage_mass_kg = 0.010;
    samples[2].time_s = 20.0;
    samples[2].beverage_mass_kg = 0.030;

    REQUIRE(interpolate_beverage_mass_g(samples, 5.0) == Catch::Approx(5.0));
    REQUIRE(interpolate_beverage_mass_g(samples, 10.0) == Catch::Approx(10.0));
    REQUIRE(interpolate_beverage_mass_g(samples, 15.0) == Catch::Approx(20.0));

    // Outside the range the cup holds its value: a shot that stopped early must
    // pay for the mass it never delivered.
    REQUIRE(interpolate_beverage_mass_g(samples, -1.0) == Catch::Approx(0.0));
    REQUIRE(interpolate_beverage_mass_g(samples, 99.0) == Catch::Approx(30.0));
    REQUIRE(interpolate_beverage_mass_g({}, 5.0) == 0.0);
}

TEST_CASE("a perfect match scores zero loss", "[calibration]") {
    const ModelCoefficients truth = testing::baseline_coefficients();
    const MeasuredShot shot = synthesize(testing::baseline_recipe(), truth, "perfect");

    const LossBreakdown loss = evaluate_shot_loss(shot, truth, SimulationConfig{}, LossWeights{});
    REQUIRE(loss.simulated);
    REQUIRE(loss.mass_rmse_g < 1.0e-9);
    REQUIRE(loss.time_error_s < 1.0e-9);
    REQUIRE(loss.tds_error_percent < 1.0e-9);
    REQUIRE(loss.total < 1.0e-9);
}

TEST_CASE("loss grows as coefficients move away from the truth", "[calibration]") {
    const ModelCoefficients truth = testing::baseline_coefficients();
    const MeasuredShot shot = synthesize(testing::baseline_recipe(), truth, "reference");

    const auto loss_at = [&](double kozeny) {
        return evaluate_shot_loss(shot, with_parameter(truth, "kozeny_constant", kozeny),
                                  SimulationConfig{}, LossWeights{})
            .total;
    };

    const double exact = loss_at(truth.kozeny_constant);
    const double near = loss_at(truth.kozeny_constant * 1.3);
    const double far = loss_at(truth.kozeny_constant * 3.0);

    REQUIRE(exact < near);
    REQUIRE(near < far);
}

TEST_CASE("an unsimulatable candidate scores finite and large", "[calibration]") {
    // The optimiser must be able to walk away from a nonphysical candidate
    // rather than propagating an exception out of the fit.
    ModelCoefficients broken = testing::baseline_coefficients();
    broken.extractable_solids_fraction = 5.0;  // outside its validation range

    const MeasuredShot shot =
        synthesize(testing::baseline_recipe(), testing::baseline_coefficients(), "reference");
    const LossBreakdown loss = evaluate_shot_loss(shot, broken, SimulationConfig{}, LossWeights{});

    REQUIRE_FALSE(loss.simulated);
    REQUIRE(std::isfinite(loss.total));
    REQUIRE(loss.total > 1.0e6);
}

TEST_CASE("only named coefficients are fittable", "[calibration]") {
    const ModelCoefficients coefficients = testing::baseline_coefficients();

    REQUIRE(tunable_parameter("kozeny_constant").has_value());
    REQUIRE(tunable_parameter("kozeny_constant")->logarithmic);
    REQUIRE_FALSE(tunable_parameter("outlet_pressure_pa").has_value());
    REQUIRE_FALSE(tunable_parameter("nonsense").has_value());

    REQUIRE(read_parameter(coefficients, "kozeny_constant") ==
            Catch::Approx(coefficients.kozeny_constant));
    REQUIRE(with_parameter(coefficients, "grind_exponent", 1.7).grind_exponent ==
            Catch::Approx(1.7));

    REQUIRE_THROWS_AS(read_parameter(coefficients, "nonsense"), InvalidInputError);
    REQUIRE_THROWS_AS(with_parameter(coefficients, "nonsense", 1.0), InvalidInputError);
}

TEST_CASE("measured shot validation rejects malformed series", "[calibration]") {
    MeasuredShot shot;
    shot.recipe = testing::baseline_recipe();
    shot.series = {{0.0, 0.0, {}}, {5.0, 10.0, {}}, {5.0, 12.0, {}}};

    const ValidationResult unordered = shot.validate();
    REQUIRE_FALSE(unordered.ok());
    REQUIRE(unordered.issues().front().code == "UNORDERED_SERIES");

    MeasuredShot negative;
    negative.recipe = testing::baseline_recipe();
    negative.series = {{0.0, -1.0, {}}};
    REQUIRE_FALSE(negative.validate().ok());

    MeasuredShot empty;
    empty.recipe = testing::baseline_recipe();
    REQUIRE_FALSE(empty.validate().ok());

    MeasuredShot invalid_final;
    invalid_final.recipe = testing::baseline_recipe();
    invalid_final.series = {{0.0, 0.0, 9.0}};
    invalid_final.final_shot_time_s = -1.0;
    REQUIRE_FALSE(invalid_final.validate().ok());
}

TEST_CASE("a fit needs parameters and shots", "[calibration]") {
    CalibrationSpec spec;
    spec.starting_point = testing::baseline_coefficients();
    REQUIRE_THROWS_AS(fit(spec), InvalidInputError);

    spec.parameters.push_back(*tunable_parameter("kozeny_constant"));
    REQUIRE_THROWS_AS(fit(spec), InvalidInputError);  // still no shots
}

TEST_CASE("a calibration spec validates its controls and parameters", "[calibration]") {
    const ModelCoefficients truth = testing::baseline_coefficients();
    CalibrationSpec spec;
    spec.starting_point = truth;
    spec.fitting_shots = {synthesize(testing::baseline_recipe(), truth, "reference")};
    spec.parameters = {{"kozeny_constant", 1.0, 1.0, true}};
    spec.maximum_iterations = 0;
    spec.weights.mass = -1.0;

    REQUIRE_THROWS_AS(fit(spec), InvalidInputError);
}
