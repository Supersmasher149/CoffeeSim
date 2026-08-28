#include "sweep_batch_runner.hpp"

#include <algorithm>
#include <atomic>
#include <optional>
#include <thread>
#include <vector>

#include "espressolab/ring_buffer.hpp"
#include "espressolab/simulator.hpp"

namespace espressolab::cli_workflows {
namespace {

std::size_t resolve_worker_count(const BatchRunnerOptions& options, std::size_t total) {
    const std::size_t requested =
        options.worker_count.value_or(std::max<std::size_t>(1, std::thread::hardware_concurrency()));
    return std::max<std::size_t>(1, std::min(requested, total));
}

}  // namespace

SweepResult run_sweep_parallel(const SweepSpec& spec, const BatchRunnerOptions& options,
                               const SweepProgressCallback& on_progress) {
    // Same structural checks as the sequential path, run once up front so a
    // malformed spec fails before any worker thread is spawned.
    validate_sweep_spec(spec);

    const std::size_t total = sweep_total_runs(spec);
    const std::size_t worker_count = resolve_worker_count(options, total);
    // This is exactly the fixed heuristic issue #38 asks to make overridable:
    // it now lives here as a default, resolved once, rather than being baked
    // into any type.
    const std::size_t ring_capacity = options.ring_capacity.value_or(worker_count * 4);

    SweepResult result;
    result.name = spec.name;
    result.axes = spec.axes;
    result.sweep_id = "sweep-" + spec.name;

    const Simulator simulator;
    BoundedRingBuffer<SweepRun> ring(ring_capacity);

    std::atomic<std::size_t> next_index{0};
    std::atomic<bool> cancel_requested{false};
    std::atomic<std::size_t> producers_remaining{worker_count};

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t w = 0; w < worker_count; ++w) {
        workers.emplace_back([&] {
            while (true) {
                const std::size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
                if (index >= total || cancel_requested.load(std::memory_order_relaxed)) break;
                ring.push(execute_sweep_point(spec, simulator, index));
            }
            // Last producer to finish is the one that closes the ring, so the
            // consumer's pop() only ever sees end-of-stream once every
            // producer has stopped pushing.
            if (producers_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) ring.close();
        });
    }

    // Slots are keyed by sweep index, not arrival order, so scheduling never
    // affects which run ends up where -- this is the determinism guarantee.
    std::vector<std::optional<SweepRun>> slots(total);
    std::size_t drained = 0;
    SweepRun run;
    while (ring.pop(run)) {
        const std::size_t index = static_cast<std::size_t>(run.index);
        slots[index] = std::move(run);
        ++drained;
        if (on_progress && !on_progress(static_cast<int>(drained), static_cast<int>(total))) {
            cancel_requested.store(true, std::memory_order_relaxed);
        }
    }

    for (auto& worker : workers) worker.join();

    // Compact in ascending index order, stopping at the first run that never
    // arrived. For a completed sweep every slot is filled, so this appends
    // all `total` runs in the same order the sequential path would. For a
    // cancelled sweep it yields an unbroken ascending-index prefix -- the
    // same "keep the runs already finished" contract as the sequential path,
    // just without a guarantee on which specific runs raced to completion
    // before the cancellation was observed.
    result.runs.reserve(drained);
    for (auto& slot : slots) {
        if (!slot) break;
        result.runs.push_back(std::move(*slot));
    }
    result.cancelled = result.runs.size() < total;

    return result;
}

}  // namespace espressolab::cli_workflows
