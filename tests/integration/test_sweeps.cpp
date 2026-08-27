#include <algorithm>
#include <fstream>

#include <catch_amalgamated.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/artifact_io.hpp"
#include "espressolab/experiment.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;

namespace {

std::filesystem::path write_temp_sweep_spec(const std::string& contents) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "espressolab-sweep-spec-test.json";
    std::ofstream stream(path, std::ios::trunc);
    stream << contents;
    return path;
}

}  // namespace

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

TEST_CASE("duplicate sweep axes are rejected before any run", "[sweep]") {
    SweepSpec spec;
    spec.baseline = testing::baseline_recipe();
    spec.coefficients = testing::baseline_coefficients();
    spec.axes = {{"puck.particle_diameter_um", {300.0}}, {"puck.particle_diameter_um", {400.0}}};

    REQUIRE_THROWS_MATCHES(
        ExperimentRunner().run(spec), InvalidInputError,
        Catch::Matchers::Predicate<InvalidInputError>([](const InvalidInputError& e) {
            return e.validation().issues().front().code == "DUPLICATE_SWEEP_AXIS";
        }));
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

// Audit F4, issue #6: a sweep spec whose JSON root is the wrong type (e.g.
// `[]`) passed parsing but then threw an uncaught nlohmann::json::type_error
// on the first `.value()` call in load_sweep_spec_file() instead of a
// structured MALFORMED_JSON error.
TEST_CASE("a sweep spec with a non-object root is a structured error", "[sweep][artifacts]") {
    const std::filesystem::path path = write_temp_sweep_spec("[]");
    REQUIRE_THROWS_MATCHES(
        artifact_io_sweep::load_sweep_spec_file(path), artifact_io::LoadError,
        Catch::Matchers::Predicate<artifact_io::LoadError>(
            [](const artifact_io::LoadError& e) { return e.code == "MALFORMED_JSON"; }));
}

TEST_CASE("a sweep spec with a wrongly typed field is a structured error", "[sweep][artifacts]") {
    // baseline_recipe is a number instead of a path string: root.at(...)
    // .get<std::string>() threw an uncaught nlohmann::json::type_error
    // before this fix's boundary translation caught it.
    const std::filesystem::path path = write_temp_sweep_spec(R"({"baseline_recipe": 123, "axes": []})");
    REQUIRE_THROWS_MATCHES(
        artifact_io_sweep::load_sweep_spec_file(path), artifact_io::LoadError,
        Catch::Matchers::Predicate<artifact_io::LoadError>(
            [](const artifact_io::LoadError& e) { return e.code == "MALFORMED_JSON"; }));
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

// Section 15.2 restores background sweep jobs. The engine stays single-threaded
// and knows nothing about who is calling it: progress and cancellation are a
// callback, and the server supplies the thread.
TEST_CASE("a sweep reports progress once per run", "[sweep][progress]") {
    SweepSpec spec;
    spec.baseline = testing::baseline_recipe();
    spec.coefficients = testing::baseline_coefficients();
    spec.axes.push_back({"puck.particle_diameter_um", {280.0, 320.0, 360.0, 400.0, 440.0}});

    std::vector<int> completed_seen;
    int total_seen = 0;
    const SweepResult result = ExperimentRunner().run(spec, [&](int completed, int total) {
        completed_seen.push_back(completed);
        total_seen = total;
        return true;
    });

    REQUIRE_FALSE(result.cancelled);
    REQUIRE(result.runs.size() == 5);
    REQUIRE(total_seen == 5);
    REQUIRE(completed_seen == std::vector<int>{1, 2, 3, 4, 5});
}

TEST_CASE("returning false stops the sweep and keeps finished runs", "[sweep][progress]") {
    SweepSpec spec;
    spec.baseline = testing::baseline_recipe();
    spec.coefficients = testing::baseline_coefficients();
    SweepAxis axis;
    axis.parameter_path = "puck.particle_diameter_um";
    for (double um = 260.0; um <= 460.0; um += 10.0) axis.values.push_back(um);
    spec.axes.push_back(axis);

    const SweepResult result = ExperimentRunner().run(spec, [](int completed, int) {
        return completed < 4;  // stop once four runs are done
    });

    REQUIRE(result.cancelled);
    REQUIRE(result.runs.size() == 4);
    // The runs it did finish are ordinary, complete results.
    for (const auto& run : result.runs) {
        REQUIRE(run.summary.termination != TerminationReason::numerical_failure);
        REQUIRE(std::isfinite(run.summary.extraction_yield_fraction));
    }
    REQUIRE(result.runs.front().coordinates.front() == Catch::Approx(260.0));
}

TEST_CASE("cancelling at the first callback keeps exactly one run", "[sweep][progress]") {
    SweepSpec spec;
    spec.baseline = testing::baseline_recipe();
    spec.coefficients = testing::baseline_coefficients();
    spec.axes.push_back({"puck.particle_diameter_um", {300.0, 350.0, 400.0}});

    const SweepResult result = ExperimentRunner().run(spec, [](int, int) { return false; });

    REQUIRE(result.cancelled);
    REQUIRE(result.runs.size() == 1);
}

TEST_CASE("a cancelled sweep still exports its partial results", "[sweep][progress]") {
    SweepSpec spec;
    spec.baseline = testing::baseline_recipe();
    spec.coefficients = testing::baseline_coefficients();
    spec.axes.push_back({"puck.particle_diameter_um", {300.0, 340.0, 380.0, 420.0}});

    const SweepResult result =
        ExperimentRunner().run(spec, [](int completed, int) { return completed < 2; });

    const std::string csv = artifact_io_sweep::dump_aggregate_csv(result);
    REQUIRE(std::count(csv.begin(), csv.end(), '\n') == 3);  // header + 2 runs

    const std::string json = artifact_io_sweep::dump_sweep_json(result);
    REQUIRE(json.find("\"cancelled\": true") != std::string::npos);
}

TEST_CASE("a sweep without a callback behaves exactly as before", "[sweep][progress]") {
    SweepSpec spec;
    spec.baseline = testing::baseline_recipe();
    spec.coefficients = testing::baseline_coefficients();
    spec.axes.push_back({"puck.particle_diameter_um", {300.0, 350.0, 400.0}});

    const SweepResult with_callback =
        ExperimentRunner().run(spec, [](int, int) { return true; });
    const SweepResult without = ExperimentRunner().run(spec);

    REQUIRE(without.runs.size() == with_callback.runs.size());
    REQUIRE_FALSE(without.cancelled);
    for (std::size_t i = 0; i < without.runs.size(); ++i) {
        REQUIRE(without.runs[i].result_hash == with_callback.runs[i].result_hash);
    }
}
