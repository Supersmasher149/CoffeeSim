#include <catch_amalgamated.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/experiment.hpp"
#include "sweep_batch_runner.hpp"

// Issue #38: the parallel sweep batch runner (worker threads + a tunable
// BoundedRingBuffer) must be a pure performance knob -- same runs, same
// order, same result_hash as ExperimentRunner's sequential path, for any
// worker_count/ring_capacity. That equivalence, not raw throughput, is what
// these tests check.
using namespace espressolab;
using namespace espressolab::cli_workflows;

namespace {

// Matches the "at least 100 runs" sweep in tests/integration/test_sweeps.cpp
// so multiple workers and a small ring actually interleave, not just a
// couple of points.
SweepSpec large_sweep_spec() {
    SweepSpec spec;
    spec.name = "batch-runner";
    spec.baseline = testing::baseline_recipe();
    spec.coefficients = testing::baseline_coefficients();

    SweepAxis grind;
    grind.parameter_path = "puck.particle_diameter_um";
    for (double um = 280.0; um <= 460.0; um += 20.0) grind.values.push_back(um);  // 10
    SweepAxis temperature;
    temperature.parameter_path = "temperature_profile_c.constant";
    for (double c = 88.0; c <= 96.0; c += 0.8) temperature.values.push_back(c);  // 11
    spec.axes = {temperature, grind};
    return spec;
}

}  // namespace

TEST_CASE("the parallel batch runner matches the sequential path's result hashes", "[sweep]") {
    const SweepSpec spec = large_sweep_spec();
    const SweepResult sequential = ExperimentRunner().run(spec);
    REQUIRE(sequential.runs.size() >= 100);

    struct Config {
        std::size_t workers;
        std::size_t ring_capacity;
    };
    for (const Config& config : {Config{1, 1}, Config{4, 2}, Config{8, 64}}) {
        BatchRunnerOptions options;
        options.worker_count = config.workers;
        options.ring_capacity = config.ring_capacity;
        const SweepResult parallel = run_sweep_parallel(spec, options);

        INFO("workers=" << config.workers << " ring_capacity=" << config.ring_capacity);
        REQUIRE_FALSE(parallel.cancelled);
        REQUIRE(parallel.runs.size() == sequential.runs.size());
        for (std::size_t i = 0; i < sequential.runs.size(); ++i) {
            REQUIRE(parallel.runs[i].index == sequential.runs[i].index);
            REQUIRE(parallel.runs[i].result_hash == sequential.runs[i].result_hash);
        }
    }
}

TEST_CASE("cancelling the parallel batch runner keeps an unbroken index prefix", "[sweep]") {
    const SweepSpec spec = large_sweep_spec();
    const std::size_t total = spec.axes[0].values.size() * spec.axes[1].values.size();

    BatchRunnerOptions options;
    options.worker_count = 4;
    options.ring_capacity = 4;  // small on purpose, to force producers to block on the consumer
    const SweepResult result = run_sweep_parallel(spec, options, [](int completed, int) {
        return completed < 10;  // stop once ten runs have arrived at the consumer
    });

    REQUIRE(result.cancelled);
    REQUIRE(result.runs.size() >= 1);
    REQUIRE(result.runs.size() < total);
    // Arrival order at the consumer need not match index order under
    // concurrency, but the exported result must still be a clean,
    // gap-free prefix -- exactly what a sequential cancellation produces.
    for (std::size_t i = 0; i < result.runs.size(); ++i) {
        REQUIRE(result.runs[i].index == static_cast<int>(i));
    }
}

TEST_CASE("a ring smaller than the worker count still delivers every run, in order", "[sweep]") {
    const SweepSpec spec = large_sweep_spec();
    const std::size_t total = spec.axes[0].values.size() * spec.axes[1].values.size();

    BatchRunnerOptions options;
    options.worker_count = 8;
    options.ring_capacity = 2;  // deliberately smaller than worker_count
    const SweepResult result = run_sweep_parallel(spec, options);

    REQUIRE_FALSE(result.cancelled);
    REQUIRE(result.runs.size() == total);
    for (std::size_t i = 0; i < result.runs.size(); ++i) {
        REQUIRE(result.runs[i].index == static_cast<int>(i));
    }
}

TEST_CASE("an out-of-range corner is recorded, not thrown, by the parallel batch runner",
         "[sweep]") {
    SweepSpec spec;
    spec.baseline = testing::baseline_recipe();
    spec.coefficients = testing::baseline_coefficients();
    // 900 um is outside the supported 150-800 um range, exactly like the
    // equivalent sequential-path test in test_sweeps.cpp.
    spec.axes.push_back({"puck.particle_diameter_um", {300.0, 400.0, 900.0}});

    BatchRunnerOptions options;
    options.worker_count = 3;
    const SweepResult result = run_sweep_parallel(spec, options);

    REQUIRE(result.runs.size() == 3);
    REQUIRE(result.runs[2].summary.termination == TerminationReason::invalid_state);
    REQUIRE(result.runs[2].run_id == "invalid");
}

TEST_CASE("an invalid spec is rejected before any worker thread starts", "[sweep]") {
    SweepSpec spec;
    spec.baseline = testing::baseline_recipe();
    spec.coefficients = testing::baseline_coefficients();
    spec.axes = {{"puck.particle_diameter_um", {300.0}}, {"puck.particle_diameter_um", {400.0}}};

    REQUIRE_THROWS_AS(run_sweep_parallel(spec), InvalidInputError);
}
