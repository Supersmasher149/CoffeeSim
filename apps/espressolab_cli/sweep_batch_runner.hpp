#pragma once

#include <cstddef>
#include <optional>

#include "espressolab/experiment.hpp"

// The one place in this codebase that owns worker threads for sweep
// execution (issue #38). engine/experiment_runner stays single-threaded and
// knows nothing about this file; it only supplies the pure, stateless
// building blocks (execute_sweep_point et al., declared in experiment.hpp)
// that both the sequential ExperimentRunner::run and this parallel path
// call, so the two produce byte-identical results. See
// include/espressolab/ring_buffer.hpp for the bounded queue that decouples
// producer worker threads from the consumer that aggregates results here.
namespace espressolab::cli_workflows {

struct BatchRunnerOptions {
    // Unset => std::thread::hardware_concurrency(), clamped to [1, total
    // sweep runs].
    std::optional<std::size_t> worker_count;
    // Unset => worker_count * 4, the fixed heuristic issue #38 asks to make
    // overridable. Resolved once per run_sweep_parallel call and fixed for
    // that call's lifetime -- no adaptive/dynamic resizing, which is
    // explicitly out of scope for this change.
    std::optional<std::size_t> ring_capacity;
};

// Runs spec's sweep points across multiple worker threads, decoupled from
// the consumer (this function itself, running on the caller's thread) by a
// BoundedRingBuffer. Produces the exact same SweepResult -- same runs, same
// order, same result_hash per run -- as ExperimentRunner().run(spec,
// on_progress) would for the same spec, regardless of worker_count or
// ring_capacity: concurrency only changes arrival order at the consumer,
// never a run's content or its final position in result.runs. Throws
// InvalidInputError for a structurally invalid spec, before any worker
// thread is spawned.
SweepResult run_sweep_parallel(const SweepSpec& spec, const BatchRunnerOptions& options = {},
                               const SweepProgressCallback& on_progress = {});

}  // namespace espressolab::cli_workflows
