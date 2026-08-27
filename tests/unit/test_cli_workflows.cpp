#include <catch_amalgamated.hpp>

#include <filesystem>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/simulator.hpp"
#include "workflows.hpp"

// Shared workflow service coverage (issue #25): both the legacy CLI's
// command_* handlers and the TUI's tui_forms.cpp call these functions, so
// this file is where "the two frontends produce identical native outputs for
// identical inputs" actually gets checked, rather than asserted.
using namespace espressolab;
using namespace espressolab::cli_workflows;

namespace {

std::filesystem::path baseline_recipe_path() {
    return testing::asset_dir() / "recipes" / "baseline.json";
}

std::filesystem::path baseline_coefficients_path() {
    return testing::asset_dir() / "coefficients" / "default-v1.json";
}

}  // namespace

TEST_CASE("run_simulate matches calling Simulator::run directly", "[cli_workflows][unit]") {
    // This is the equivalence the legacy CLI and the TUI both depend on:
    // the workflow layer does not change what the solver computes.
    SimulateRequest request;
    request.recipe_path = baseline_recipe_path().string();
    request.coefficients_path = baseline_coefficients_path().string();
    const SimulateOutcome outcome = run_simulate(request);

    const Recipe recipe = testing::baseline_recipe();
    const ModelCoefficients coefficients = testing::baseline_coefficients();
    ShotResult direct = Simulator().run(recipe, coefficients, outcome.config);
    artifact_io::stamp_manifest(direct, recipe, coefficients, outcome.config);

    REQUIRE(outcome.result.manifest.result_hash == direct.manifest.result_hash);
    REQUIRE(outcome.result.summary.beverage_mass_kg ==
           Catch::Approx(direct.summary.beverage_mass_kg));
}

TEST_CASE("run_simulate is deterministic across repeated calls", "[cli_workflows][unit]") {
    SimulateRequest request;
    request.recipe_path = baseline_recipe_path().string();
    request.coefficients_path = baseline_coefficients_path().string();

    const SimulateOutcome first = run_simulate(request);
    const SimulateOutcome second = run_simulate(request);
    REQUIRE(first.result.manifest.result_hash == second.result.manifest.result_hash);
}

TEST_CASE("run_simulate writes artifacts only when an output directory is requested",
         "[cli_workflows][unit]") {
    const std::filesystem::path out_dir =
        std::filesystem::temp_directory_path() / "espressolab_cli_workflows_artifact_test";
    std::error_code ignored;
    std::filesystem::remove_all(out_dir, ignored);

    SimulateRequest request;
    request.recipe_path = baseline_recipe_path().string();
    request.coefficients_path = baseline_coefficients_path().string();
    request.out_dir = out_dir.string();

    const SimulateOutcome outcome = run_simulate(request);
    REQUIRE_FALSE(outcome.artifacts_dir.empty());
    REQUIRE(std::filesystem::exists(out_dir));
    REQUIRE_FALSE(std::filesystem::is_empty(out_dir));

    std::filesystem::remove_all(out_dir, ignored);
}

TEST_CASE("a cancelled run_simulate writes no artifacts", "[cli_workflows][unit][cancellation]") {
    const std::filesystem::path out_dir =
        std::filesystem::temp_directory_path() / "espressolab_cli_workflows_cancel_test";
    std::error_code ignored;
    std::filesystem::remove_all(out_dir, ignored);

    SimulateRequest request;
    request.recipe_path = baseline_recipe_path().string();
    request.coefficients_path = baseline_coefficients_path().string();
    request.out_dir = out_dir.string();

    REQUIRE_THROWS_AS(run_simulate(request, [] { return true; }), ExecutionCancelled);
    REQUIRE_FALSE(std::filesystem::exists(out_dir));
}

TEST_CASE("run_bench validates seconds and repeats before running", "[cli_workflows][unit]") {
    BenchRequest request;
    request.recipe_path = baseline_recipe_path().string();
    request.seconds = 0.0;
    REQUIRE_THROWS_AS(run_bench(request), InvalidInputError);

    request.seconds = 10.0;  // recipe.stop.maximum_time_s must be within [10, 60] s
    request.repeats = 0;
    REQUIRE_THROWS_AS(run_bench(request), InvalidInputError);

    request.repeats = 1;
    REQUIRE_NOTHROW(run_bench(request));
}

TEST_CASE("run_cfd3d requires a case file or a recipe file", "[cli_workflows][unit]") {
    // Regression: this used to be enforced only by main.cpp's flag parsing,
    // so a caller that builds a Cfd3dRequest directly (namely the TUI, once
    // its "recipe" field default was fixed to not mask a missing --case)
    // could reach the solver with a default-constructed, invalid recipe.
    Cfd3dRequest request;
    try {
        run_cfd3d(request);
        FAIL("expected InvalidInputError");
    } catch (const InvalidInputError& error) {
        REQUIRE(error.validation().issues().size() == 1);
        REQUIRE(error.validation().issues().front().code == "MISSING_ARGUMENT");
    }

    request.recipe_path = baseline_recipe_path().string();
    request.nx = 6;
    request.ny = 6;
    request.nz = 8;
    request.dt_s = 0.02;
    REQUIRE_NOTHROW(run_cfd3d(request));
}

TEST_CASE("run_calibrate maps an unknown fit parameter to the legacy error code",
         "[cli_workflows][unit]") {
    CalibrateRequest request;
    request.shots_dir = (testing::asset_dir() / "measured_shots").string();
    request.fit_names = {"not_a_real_coefficient"};

    try {
        run_calibrate(request);
        FAIL("expected InvalidInputError");
    } catch (const InvalidInputError& error) {
        REQUIRE(error.validation().issues().size() == 1);
        REQUIRE(error.validation().issues().front().code == "UNKNOWN_PARAMETER_NAME");
    }
}
