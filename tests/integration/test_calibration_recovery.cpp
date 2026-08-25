#include <catch_amalgamated.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/artifact_io.hpp"
#include "espressolab/calibration.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;
using namespace espressolab::calibration;

namespace {

MeasuredShot synthesize(const Recipe& recipe, const ModelCoefficients& coefficients,
                        const std::string& id) {
    const ShotResult result = Simulator().run(recipe, coefficients);

    MeasuredShot shot;
    shot.id = id;
    shot.source_stem = id;
    shot.recipe = recipe;
    shot.synthetic = true;

    double next = 0.0;
    for (const auto& sample : result.samples) {
        if (sample.time_s + 1.0e-9 < next) continue;
        next += 0.2;
        shot.series.push_back({sample.time_s, units::kg_to_grams(sample.beverage_mass_kg), {}});
    }
    shot.final_beverage_mass_g = units::kg_to_grams(result.summary.beverage_mass_kg);
    shot.final_shot_time_s = result.summary.elapsed_time_s;
    shot.final_tds_percent = result.summary.tds_fraction * 100.0;
    return shot;
}

CalibrationSpec recovery_spec(const ModelCoefficients& truth, const ModelCoefficients& start) {
    CalibrationSpec spec;
    spec.starting_point = start;
    spec.parameters = {*tunable_parameter("kozeny_constant"),
                       *tunable_parameter("extraction_rate_ref_s")};
    spec.fitting_shots = {
        synthesize(testing::baseline_recipe(), truth, "baseline"),
        synthesize(artifact_io::load_recipe_file(testing::asset_dir() / "recipes" /
                                                 "pre-infusion.json"),
                   truth, "pre-infusion")};
    spec.validation_shots = {
        synthesize(artifact_io::load_recipe_file(testing::asset_dir() / "recipes" /
                                                 "immediate-pressure.json"),
                   truth, "immediate")};
    return spec;
}

}  // namespace

// The honest test of a calibration routine without real data: hide known
// coefficients inside synthetic shots and check the fitter finds them again.
// This validates the machinery, not the model.
TEST_CASE("the fit recovers coefficients it was not given", "[calibration][recovery]") {
    const ModelCoefficients truth = testing::baseline_coefficients();

    ModelCoefficients start = truth;
    start.kozeny_constant = truth.kozeny_constant * 3.0;
    start.extraction_rate_ref_s = truth.extraction_rate_ref_s * 0.4;

    const CalibrationReport report = fit(recovery_spec(truth, start));

    REQUIRE(report.converged);
    REQUIRE(report.final_loss < report.starting_loss);
    REQUIRE(report.final_loss < 1.0e-3);

    REQUIRE(report.fitted.kozeny_constant ==
            Catch::Approx(truth.kozeny_constant).epsilon(0.02));
    REQUIRE(report.fitted.extraction_rate_ref_s ==
            Catch::Approx(truth.extraction_rate_ref_s).epsilon(0.02));

    // Coefficients that were not in the parameter list must not have moved.
    REQUIRE(report.fitted.initial_porosity == Catch::Approx(truth.initial_porosity));
    REQUIRE(report.fitted.grind_exponent == Catch::Approx(truth.grind_exponent));
}

TEST_CASE("a recovered fit generalises to the held-out shot", "[calibration][recovery]") {
    const ModelCoefficients truth = testing::baseline_coefficients();
    ModelCoefficients start = truth;
    start.kozeny_constant = truth.kozeny_constant * 3.0;
    start.extraction_rate_ref_s = truth.extraction_rate_ref_s * 0.4;

    const CalibrationReport report = fit(recovery_spec(truth, start));

    REQUIRE(report.validation_losses.size() == 1);
    REQUIRE(report.validation_losses.front().shot_id == "immediate");
    // The held-out shot must be reproduced about as well as the fitting shots.
    REQUIRE(report.validation_loss < 1.0e-3);
    REQUIRE(report.validation_losses.front().loss.mass_rmse_g < 0.05);
}

TEST_CASE("synthetic data is flagged all the way into the report", "[calibration]") {
    const ModelCoefficients truth = testing::baseline_coefficients();
    ModelCoefficients start = truth;
    start.kozeny_constant = truth.kozeny_constant * 1.5;

    CalibrationSpec spec = recovery_spec(truth, start);
    spec.maximum_iterations = 20;
    const CalibrationReport report = fit(spec);

    REQUIRE(report.used_synthetic_data);

    const std::string json = io::dump_report_json(report);
    REQUIRE(json.find("warning_synthetic") != std::string::npos);

    const std::string coefficients = io::dump_fitted_coefficients_json(
        report, "fitted-test", "0.0.1", {"baseline", "pre-infusion"}, {"immediate"});
    REQUIRE(coefficients.find("SYNTHETIC DATA") != std::string::npos);
}

TEST_CASE("a fit with no held-out shot says so in writing", "[calibration]") {
    const ModelCoefficients truth = testing::baseline_coefficients();
    CalibrationSpec spec = recovery_spec(truth, truth);
    spec.validation_shots.clear();
    spec.maximum_iterations = 10;

    const CalibrationReport report = fit(spec);
    REQUIRE(report.validation_losses.empty());

    const std::string json = io::dump_report_json(report);
    REQUIRE(json.find("No held-out validation shot") != std::string::npos);

    const std::string coefficients =
        io::dump_fitted_coefficients_json(report, "fitted-test", "0.0.1", {"baseline"}, {});
    REQUIRE(coefficients.find("has not been shown to generalise") != std::string::npos);
}

TEST_CASE("the same specification produces the same fit", "[calibration][recovery]") {
    // Determinism carries into calibration: a coefficient file has to be
    // reproducible from its inputs like everything else (10.2).
    const ModelCoefficients truth = testing::baseline_coefficients();
    ModelCoefficients start = truth;
    start.kozeny_constant = truth.kozeny_constant * 2.0;
    start.extraction_rate_ref_s = truth.extraction_rate_ref_s * 0.6;

    const CalibrationReport first = fit(recovery_spec(truth, start));
    const CalibrationReport second = fit(recovery_spec(truth, start));

    REQUIRE(first.iterations == second.iterations);
    REQUIRE(first.simulations == second.simulations);
    REQUIRE(first.final_loss == second.final_loss);
    REQUIRE(first.fitted_values == second.fitted_values);
}

TEST_CASE("held-out shots never steer the fit", "[calibration][recovery]") {
    const ModelCoefficients truth = testing::baseline_coefficients();
    ModelCoefficients start = truth;
    start.kozeny_constant = truth.kozeny_constant * 2.5;

    CalibrationSpec with_holdout = recovery_spec(truth, start);
    with_holdout.parameters = {*tunable_parameter("kozeny_constant")};

    CalibrationSpec without_holdout = with_holdout;
    without_holdout.validation_shots.clear();

    const CalibrationReport a = fit(with_holdout);
    const CalibrationReport b = fit(without_holdout);

    REQUIRE(a.fitted_values == b.fitted_values);
    REQUIRE(a.final_loss == b.final_loss);
}
