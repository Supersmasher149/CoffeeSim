#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "espressolab/result.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/types.hpp"

// Section 11: sweeps run headlessly. At least 100 runs must complete without a
// browser (FR-05).
namespace espressolab {

// Appendix-style contract from 11.2.
struct SweepAxis {
    std::string parameter_path;  // e.g. "puck.particle_diameter_um"
    std::vector<double> values;
};

struct SweepSpec {
    std::string name = "sweep";
    Recipe baseline;
    ModelCoefficients coefficients;
    SimulationConfig config;
    std::vector<SweepAxis> axes;
    std::vector<std::string> output_metrics = {"beverage_mass_g", "shot_time_s",
                                               "tds_percent", "extraction_yield_percent"};
};

struct SweepRun {
    int index = 0;
    std::vector<double> coordinates;  // one value per axis, in axis order
    ShotSummary summary;
    std::string run_id;
    std::string result_hash;
    int warning_count = 0;
};

struct SweepResult {
    std::string sweep_id;
    std::string name;
    std::vector<SweepAxis> axes;
    std::vector<SweepRun> runs;
    // True when the caller stopped the sweep early. The runs already completed
    // are still valid and still exported.
    bool cancelled = false;
};

// Called after each run with (completed, total). Returning false stops the
// sweep. The engine owns no threads: whoever wants a sweep in the background
// runs it on their own thread and answers this callback (section 3.4).
using SweepProgressCallback = std::function<bool(int, int)>;

// Applies one sweep coordinate to a copy of the baseline recipe. Throws
// InvalidInputError for an unknown parameter path.
Recipe apply_parameter(const Recipe& baseline, const std::string& parameter_path, double value);
std::vector<std::string> supported_parameter_paths();

class ExperimentRunner {
public:
    // Cartesian product of the axes, in declared order, so run ordering is
    // stable across machines (14.2).
    [[nodiscard]] SweepResult run(const SweepSpec& spec,
                                  const SweepProgressCallback& on_progress = {}) const;
};

namespace artifact_io_sweep {
std::string dump_sweep_json(const SweepResult& result, int indent = 2);
std::string dump_runs_jsonl(const SweepResult& result);
std::string dump_aggregate_csv(const SweepResult& result);
SweepSpec load_sweep_spec_file(const std::filesystem::path& file);
void write_sweep_artifacts(const std::filesystem::path& directory, const SweepResult& result);
}  // namespace artifact_io_sweep

}  // namespace espressolab
