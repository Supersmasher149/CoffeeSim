#include <catch_amalgamated.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/experiment.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;

// Section 14.2: a sweep contains the expected run count with stable parameter
// ordering. FR-05: at least 100 runs complete without a browser.
TEST_CASE("a one-dimensional sweep runs headlessly with stable ordering", "[sweep]") {
    SweepSpec spec;
    spec.name = "grind";
    spec.baseline = testing::baseline_recipe();
    spec.coefficients = testing::baseline_coefficients();
    SweepAxis axis;
    axis.parameter_path = "puck.particle_diameter_um";
    for (double um = 250.0; um <= 450.0; um += 25.0) axis.values.push_back(um);
    spec.axes.push_back(axis);

    const SweepResult result = ExperimentRunner().run(spec);
    REQUIRE(result.runs.size() == axis.values.size());

    for (std::size_t i = 0; i < result.runs.size(); ++i) {
        REQUIRE(result.runs[i].index == static_cast<int>(i));
        REQUIRE(result.runs[i].coordinates.front() == Catch::Approx(axis.values[i]));
    }

    // Physical expectation: coarser grind, shorter shot, weaker result.
    REQUIRE(result.runs.front().summary.tds_fraction > result.runs.back().summary.tds_fraction);
    REQUIRE(result.runs.back().summary.elapsed_time_s <
            result.runs.front().summary.elapsed_time_s);
}

TEST_CASE("at least 100 runs complete without a browser", "[sweep]") {
    SweepSpec spec;
    spec.name = "large";
    spec.baseline = testing::baseline_recipe();
    spec.coefficients = testing::baseline_coefficients();

    SweepAxis grind;
    grind.parameter_path = "puck.particle_diameter_um";
    for (double um = 280.0; um <= 460.0; um += 20.0) grind.values.push_back(um);  // 10
    SweepAxis temperature;
    temperature.parameter_path = "temperature_profile_c.constant";
    for (double c = 88.0; c <= 96.0; c += 0.8) temperature.values.push_back(c);   // 11
    spec.axes = {temperature, grind};

    const SweepResult result = ExperimentRunner().run(spec);
    REQUIRE(result.runs.size() >= 100);
    for (const auto& run : result.runs) {
        REQUIRE(run.summary.termination != TerminationReason::numerical_failure);
        REQUIRE(std::isfinite(run.summary.extraction_yield_fraction));
    }

    // Last axis varies fastest, so the first two runs share a temperature.
    REQUIRE(result.runs[0].coordinates[0] == Catch::Approx(result.runs[1].coordinates[0]));
    REQUIRE(result.runs[0].coordinates[1] < result.runs[1].coordinates[1]);
}

TEST_CASE("an unknown parameter path fails before any run", "[sweep]") {
    SweepSpec spec;
    spec.baseline = testing::baseline_recipe();
    spec.coefficients = testing::baseline_coefficients();
    spec.axes.push_back({"puck.grinder_dial_setting", {4.0, 5.0}});

    REQUIRE_THROWS_AS(ExperimentRunner().run(spec), InvalidInputError);
}

TEST_CASE("sweep parameters apply in the recipe's own units", "[sweep]") {
    const Recipe baseline = testing::baseline_recipe();

    const Recipe grind = apply_parameter(baseline, "puck.particle_diameter_um", 420.0);
    REQUIRE(units::m_to_microns(grind.particle_diameter_m) == Catch::Approx(420.0));

    const Recipe hotter = apply_parameter(baseline, "temperature_profile_c.constant", 96.0);
    REQUIRE(units::kelvin_to_celsius(hotter.inlet_temperature_k.sample(12.0)) ==
            Catch::Approx(96.0));

    // Scaling preserves the shape of the profile.
    const Recipe scaled = apply_parameter(baseline, "pressure_profile_bar.scale", 0.5);
    REQUIRE(units::pa_to_bar(scaled.pressure_pa.sample(0.0)) == Catch::Approx(1.0));
    REQUIRE(units::pa_to_bar(scaled.pressure_pa.sample(20.0)) == Catch::Approx(4.5));
    REQUIRE(scaled.pressure_pa.points().size() == baseline.pressure_pa.points().size());
}

TEST_CASE("an out-of-range corner is recorded instead of aborting the sweep", "[sweep]") {
    SweepSpec spec;
    spec.baseline = testing::baseline_recipe();
    spec.coefficients = testing::baseline_coefficients();
    // 900 um is outside the supported 150-800 um range.
    spec.axes.push_back({"puck.particle_diameter_um", {300.0, 400.0, 900.0}});

    const SweepResult result = ExperimentRunner().run(spec);
    REQUIRE(result.runs.size() == 3);
    REQUIRE(result.runs[2].summary.termination == TerminationReason::invalid_state);
    REQUIRE(result.runs[2].run_id == "invalid");
    REQUIRE(result.runs[0].summary.termination == TerminationReason::target_mass_reached);
}
