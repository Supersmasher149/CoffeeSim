#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "espressolab/artifact_io.hpp"
#include "espressolab/calibration.hpp"
#include "espressolab/cfd.hpp"
#include "espressolab/cfd3d.hpp"
#include "espressolab/cfd3d_artifact_io.hpp"
#include "espressolab/execution.hpp"
#include "espressolab/experiment.hpp"
#include "espressolab/simulator.hpp"

// Shared CLI workflow services (issue #25). This is the one place that loads
// files, validates them, calls the native solver/calibration/experiment APIs,
// and writes artifacts for every espressolab_cli command. Both the legacy
// flag-driven handlers in main.cpp and the guided forms in tui/ call these
// functions so the two frontends are guaranteed to produce identical native
// outputs, units, and result hashes for identical inputs (11.3/12.2 and the
// TUI architecture note in CLAUDE.md).
//
// What stays out of this layer, deliberately:
//  - Argument syntax: which CLI flags are present, or which TUI fields are
//    populated. Each frontend still owns collecting its own input.
//  - Presentation: stdout formatting for the legacy CLI vs. TUI result lines.
//  - Argument-shape usage errors that are specific to invoking the CLI with
//    argv (e.g. "a required flag is missing"): those remain a frontend
//    concern because an interactive form has no equivalent notion.
//
// Everything else -- loading, cross-field validation once inputs are in
// hand, calling the solver, and writing artifacts -- lives here, and raises
// `espressolab::InvalidInputError` or `espressolab::artifact_io::LoadError`
// with the same codes the legacy CLI has always used, so error translation
// stays centralized and exit codes do not move.
#include "espressolab/grinder.hpp"
#include "espressolab/grinder_io.hpp"

namespace espressolab::cli_workflows {

// ---------------------------------------------------------------- simulate --
struct SimulateRequest {
    std::string recipe_path;
    std::string coefficients_path;  // empty => default coefficients
    std::optional<double> dt_s;
    std::optional<double> sample_interval_s;
    std::string out_dir;  // empty => artifacts are not written
};

struct SimulateOutcome {
    Recipe recipe;
    ModelCoefficients coefficients;
    SimulationConfig config;
    ShotResult result;
    double wall_time_ms = 0.0;
    std::filesystem::path artifacts_dir;  // empty when out_dir was empty
};

SimulateOutcome run_simulate(const SimulateRequest& request,
                             const CancellationCallback& is_cancelled = {});

// -------------------------------------------------------------------- sweep --
struct SweepRequest {
    std::string spec_path;
    std::string out_dir;
    // Issue #38: unset (the default) runs ExperimentRunner's sequential
    // path, exactly as before. Set to opt into the parallel batch runner;
    // ring_capacity without workers is a frontend-level usage error (see
    // command_sweep in main.cpp) so the override is never silently ignored.
    std::optional<std::size_t> workers;
    std::optional<std::size_t> ring_capacity;
};

struct SweepOutcome {
    SweepResult result;
    double wall_time_ms = 0.0;
    std::filesystem::path artifacts_dir;
};

SweepOutcome run_sweep(const SweepRequest& request,
                       const SweepProgressCallback& on_progress = {});

// -------------------------------------------------------------- synthesize --
struct SynthesizeRequest {
    std::string recipe_path;
    std::string coefficients_path;
    double noise_g = 0.0;
    unsigned int seed = 1;
    std::string out_path;  // required by the caller
    // Legacy `--recipe-path` provenance override; empty means "use recipe_path".
    std::string recipe_path_for_provenance;
};

struct SynthesizeOutcome {
    Recipe recipe;
    ShotResult result;
    std::filesystem::path out_path;
};

SynthesizeOutcome run_synthesize(const SynthesizeRequest& request,
                                 const CancellationCallback& is_cancelled = {});

// ------------------------------------------------------------------- bench --
struct BenchRequest {
    double seconds = 60.0;
    int repeats = 200;
    std::string recipe_path = "assets/recipes/baseline.json";
    std::string coefficients_path;
};

struct BenchOutcome {
    long long steps = 0;
    std::vector<double> samples_ms;  // sorted ascending
    double best_ms = 0.0;
    double median_ms = 0.0;
    double p95_ms = 0.0;
    double dt_s = 0.0;
};

using BenchProgressCallback = std::function<void(int completed, int total)>;

BenchOutcome run_bench(const BenchRequest& request, const CancellationCallback& is_cancelled = {},
                       const BenchProgressCallback& on_progress = {});

// --------------------------------------------------------------- calibrate --
struct CalibrateRequest {
    std::string shots_dir;
    std::string coefficients_path;
    std::vector<std::string> fit_names;
    std::vector<std::string> holdout_ids;  // ignored when leave_one_out is set
    bool leave_one_out = false;
    std::optional<int> max_iterations;
    std::string report_path;
    std::string out_path;
    std::string id;
    std::string coefficient_version = "1.0.0";
};

struct CalibrateOutcome {
    calibration::CalibrationSpec spec;
    std::optional<calibration::LeaveOneOutReport> leave_one_out_report;
    std::optional<calibration::CalibrationReport> report;
    std::vector<std::string> fitting_ids;
    std::vector<std::string> validation_ids;
    double wall_time_ms = 0.0;
    std::filesystem::path report_written;
    std::filesystem::path coefficients_written;
    bool coefficients_withheld_by_validation = false;
};

CalibrateOutcome run_calibrate(const CalibrateRequest& request,
                               const CancellationCallback& is_cancelled = {});

// -------------------------------------------------------------------- cfd ---
struct CfdRequest {
    std::string recipe_path;
    std::string coefficients_path;
    std::optional<int> radial_cells;
    std::optional<int> axial_cells;
    std::optional<double> dt_s;
};

struct CfdOutcome {
    Recipe recipe;
    CfdConfig config;
    CfdResult result;
    double wall_time_ms = 0.0;
};

CfdOutcome run_cfd(const CfdRequest& request, const CancellationCallback& is_cancelled = {});

// ------------------------------------------------------------------ cfd3d ---
struct Cfd3dRequest {
    std::string case_path;  // empty => build config from recipe/coefficients
    std::string recipe_path;
    std::string coefficients_path;
    std::optional<int> nx;
    std::optional<int> ny;
    std::optional<int> nz;
    std::optional<double> dt_s;
    std::optional<double> sample_interval_s;
    std::optional<double> snapshot_interval_s;
    std::string material_path;
    std::string out_dir;  // empty => artifacts (and snapshot capture) are skipped
};

struct Cfd3dOutcome {
    cfd3d_artifact_io::Cfd3dCase cfd3d_case;
    Cfd3dResult result;
    std::vector<Cfd3dSnapshot> snapshots;
    double wall_time_ms = 0.0;
    std::filesystem::path artifacts_dir;
    std::optional<cfd3d_artifact_io::Cfd3dRunManifest> manifest;
};

Cfd3dOutcome run_cfd3d(const Cfd3dRequest& request, const CancellationCallback& is_cancelled = {});

// ----------------------------------------------------------------- grind ---
// The comminution model. Outside the shot pipeline entirely: it reads no
// recipe and writes no shot artifact.
struct GrindRequest {
    std::string spec_path;  // empty => the built-in default spec
    std::string out_path;   // empty => print only, write nothing
};

struct GrindOutcome {
    GrinderSpec spec;
    GrinderResult result;
    double wall_time_ms = 0.0;
    std::filesystem::path result_path;  // empty when nothing was written
    std::filesystem::path grind_path;
};

GrindOutcome run_grind(const GrindRequest& request);

// ---------------------------------------------------------------- helpers ---
// Shared so both frontends read/write files identically. `path` may name a
// directory or a plain file depending on the caller.
std::string read_text_file(const std::filesystem::path& path);
void write_text_file(const std::filesystem::path& path, const std::string& contents);
Cfd3dMaterialField load_cfd3d_material(const std::filesystem::path& path, const Cfd3dMesh& mesh);

}  // namespace espressolab::cli_workflows
