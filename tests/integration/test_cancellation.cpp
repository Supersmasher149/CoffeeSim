#include <catch_amalgamated.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/calibration.hpp"
#include "espressolab/cfd.hpp"
#include "espressolab/cfd3d.hpp"
#include "espressolab/execution.hpp"
#include "espressolab/simulator.hpp"

// Regression coverage for issue #31 (cooperative cancellation and status
// control): every long-running native workflow checks a thread-agnostic
// `CancellationCallback` at a safe boundary and stops with `ExecutionCancelled`
// rather than running to completion or corrupting state. These are native,
// terminal-independent tests -- no TUI or PTY involved -- exercising exactly
// the checkpoints the TUI's worker thread relies on for its cancel button and
// Ctrl-C handling.
using namespace espressolab;

namespace {

// Never returns cancelled: a control to prove a workflow completes normally
// when nothing asks it to stop.
bool never() { return false; }

// Cancelled on the very first check: proves the checkpoint fires before any
// expensive work completes, which is what makes "no incomplete artifact" true
// for a caller that gates its artifact write on successful completion.
bool always() { return true; }

CfdConfig small_cfd_config() {
    CfdConfig config;
    config.mesh.radial_cells = 4;
    config.mesh.axial_cells = 6;
    config.dt_s = 0.02;
    return config;
}

Cfd3dConfig small_cfd3d_config() {
    Cfd3dConfig config;
    config.mesh = {4, 4, 6};
    config.dt_s = 0.02;
    return config;
}

}  // namespace

TEST_CASE("Simulator::run stops cooperatively when cancelled", "[cancellation]") {
    const Recipe recipe = testing::baseline_recipe();
    const ModelCoefficients coefficients = testing::baseline_coefficients();

    REQUIRE_NOTHROW(Simulator().run(recipe, coefficients, {}, never));
    REQUIRE_THROWS_AS(Simulator().run(recipe, coefficients, {}, always), ExecutionCancelled);
}

TEST_CASE("CfdSolver::run stops cooperatively when cancelled", "[cancellation][cfd]") {
    const Recipe recipe = testing::baseline_recipe();
    const ModelCoefficients coefficients = testing::baseline_coefficients();
    const CfdConfig config = small_cfd_config();

    REQUIRE_NOTHROW(CfdSolver().run(recipe, coefficients, config, never));
    REQUIRE_THROWS_AS(CfdSolver().run(recipe, coefficients, config, always), ExecutionCancelled);
}

TEST_CASE("Cfd3dSolver::run stops cooperatively when cancelled", "[cancellation][cfd3d]") {
    const Recipe recipe = testing::baseline_recipe();
    const ModelCoefficients coefficients = testing::baseline_coefficients();
    const Cfd3dConfig config = small_cfd3d_config();

    REQUIRE_NOTHROW(Cfd3dSolver().run(recipe, coefficients, config, never));
    REQUIRE_THROWS_AS(Cfd3dSolver().run(recipe, coefficients, config, always), ExecutionCancelled);
}

TEST_CASE("calibration::fit stops cooperatively when cancelled", "[cancellation][calibration]") {
    calibration::CalibrationSpec spec;
    spec.starting_point = testing::baseline_coefficients();
    spec.maximum_iterations = 5;
    spec.parameters.push_back(*calibration::tunable_parameter("kozeny_constant"));

    calibration::MeasuredShot shot;
    shot.id = "synthetic";
    shot.source_stem = "synthetic";
    shot.recipe = testing::baseline_recipe();
    shot.synthetic = true;
    shot.final_beverage_mass_g = 36.0;
    shot.final_shot_time_s = 29.0;
    spec.fitting_shots.push_back(shot);

    // The objective function checks cancellation before every simulation, so
    // a fit that is cancelled from the first call never produces a report.
    REQUIRE_THROWS_AS(calibration::fit(spec, always), ExecutionCancelled);
}

TEST_CASE("evaluate_shot_loss stops cooperatively when cancelled", "[cancellation][calibration]") {
    calibration::MeasuredShot shot;
    shot.id = "synthetic";
    shot.source_stem = "synthetic";
    shot.recipe = testing::baseline_recipe();
    shot.synthetic = true;
    shot.final_beverage_mass_g = 36.0;
    shot.final_shot_time_s = 29.0;

    REQUIRE_THROWS_AS(
        calibration::evaluate_shot_loss(shot, testing::baseline_coefficients(), {}, {}, always),
        ExecutionCancelled);
}
