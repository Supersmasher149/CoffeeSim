#include "tui_forms.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "../workflows.hpp"
#include "espressolab/units.hpp"
#include "espressolab/version.hpp"

namespace espressolab::tui {

namespace {

using espressolab::CancellationCallback;

void append_shot_report(std::vector<std::string>& lines, const ShotResult& result) {
    const ShotSummary& summary = result.summary;
    lines.push_back("termination   " + std::string(to_string(summary.termination)));
    lines.push_back("shot time     " + format_number(summary.elapsed_time_s) + " s");
    lines.push_back("beverage mass " + format_number(units::kg_to_grams(summary.beverage_mass_kg)) + " g");
    lines.push_back("brew ratio    1:" + format_number(summary.brew_ratio));
    lines.push_back("average flow  " + format_number(units::m3_s_to_ml_s(summary.average_flow_m3_s)) + " ml/s");
    lines.push_back("peak flow     " + format_number(units::m3_s_to_ml_s(summary.peak_flow_m3_s)) + " ml/s");
    lines.push_back("TDS           " + format_number(summary.tds_fraction * 100.0) + " %");
    lines.push_back("extraction    " + format_number(summary.extraction_yield_fraction * 100.0) + " %");
    lines.push_back("mass residual water " + format_number(result.diagnostics.water_mass_residual_kg, 6) + " kg");
    lines.push_back("clamps        " + std::to_string(result.diagnostics.clamp_count));
    lines.push_back("samples       " + std::to_string(result.samples.size()));
    lines.push_back("result hash   " + result.manifest.result_hash);
    // Mirrors print_shot_report() in main.cpp. The two frontends must report the
    // same run identically, so a change to one belongs in the other.
    if (result.flavor.has_value()) {
        const espressolab::FlavorSummary& flavor = result.flavor->summary;
        lines.push_back("");
        lines.push_back("sensory estimate (heuristic, uncalibrated) for " +
                        result.flavor->bean_id);
        lines.push_back("verdict       " + std::string(to_string(flavor.verdict)));
        lines.push_back("match score   " + format_number(flavor.match_score, 1) +
                        " / 100 vs the bean's target");
        lines.push_back("furthest off  " +
                        std::string(to_string(flavor.dominant_deviation_axis)));
        for (std::size_t a = 0; a < espressolab::kSensoryAxisCount; ++a) {
            const espressolab::FlavorAxisScore& axis = flavor.axes[a];
            lines.push_back(
                "  " + std::string(to_string(static_cast<espressolab::SensoryAxis>(a))) + " " +
                format_number(axis.intensity, 1) + " target " +
                format_number(axis.target, 1) + " (" + format_number(axis.deviation, 1) + ")");
        }
    }
    for (const auto& warning : result.warnings) {
        lines.push_back("warning [" + warning.code + "] " + warning.message);
    }
}

void append_cfd_report(std::vector<std::string>& lines, const CfdResult& result) {
    lines.push_back("termination   " + std::string(to_string(result.termination)));
    lines.push_back("shot time     " + format_number(result.elapsed_time_s) + " s");
    lines.push_back("beverage mass " + format_number(units::kg_to_grams(result.beverage_mass_kg)) + " g");
    lines.push_back("TDS           " + format_number(result.tds_fraction * 100.0) + " %");
    lines.push_back("extraction    " + format_number(result.extraction_yield_fraction * 100.0) + " %");
    lines.push_back("mesh          " + std::to_string(result.mesh.radial_cells) + " x " +
                    std::to_string(result.mesh.axial_cells));
    lines.push_back("pressure resid " + format_number(result.diagnostics.pressure_residual, 5));
    lines.push_back("water residual " + format_number(result.diagnostics.water_mass_residual_kg, 6) + " kg");
    lines.push_back("solids residual " + format_number(result.diagnostics.solids_mass_residual_kg, 6) + " kg");
    lines.push_back("saturation clamps " + std::to_string(result.diagnostics.saturation_clamp_count));
    for (const auto& warning : result.warnings) lines.push_back("warning [" + warning.code + "] " + warning.message);
}

void append_cfd3d_report(std::vector<std::string>& lines, const Cfd3dResult& result, std::size_t snapshot_count) {
    lines.push_back("termination   " + std::string(to_string(result.termination)));
    lines.push_back("shot time     " + format_number(result.elapsed_time_s) + " s");
    lines.push_back("beverage mass " + format_number(units::kg_to_grams(result.beverage_mass_kg)) + " g");
    lines.push_back("TDS           " + format_number(result.tds_fraction * 100.0) + " %");
    lines.push_back("extraction    " + format_number(result.extraction_yield_fraction * 100.0) + " %");
    lines.push_back("mesh          " + std::to_string(result.mesh.nx) + " x " + std::to_string(result.mesh.ny) +
                    " x " + std::to_string(result.mesh.nz));
    lines.push_back("snapshots     " + std::to_string(snapshot_count));
    lines.push_back("pressure resid " + format_number(result.diagnostics.pressure_residual, 5));
    lines.push_back("water residual " + format_number(result.diagnostics.water_mass_residual_kg, 6) + " kg");
    lines.push_back("solids residual " + format_number(result.diagnostics.solids_mass_residual_kg, 6) + " kg");
    lines.push_back("energy residual " + format_number(result.diagnostics.energy_residual_j, 6) + " J");
    for (const auto& warning : result.warnings) lines.push_back("warning [" + warning.code + "] " + warning.message);
}

JobResult run_simulate(const std::vector<Field>& fields, const CancellationCallback& cancel, const ProgressCallback&) {
    cli_workflows::SimulateRequest request;
    request.recipe_path = field_value(fields, "recipe");
    request.coefficients_path = field_value(fields, "coefficients");
    request.dt_s = parse_double(fields, "dt");
    request.sample_interval_s = parse_double(fields, "sample interval");
    request.out_dir = field_value(fields, "out");

    const cli_workflows::SimulateOutcome outcome = cli_workflows::run_simulate(request, cancel);

    JobResult output;
    output.lines.push_back("recipe: " + outcome.recipe.name);
    output.lines.push_back("solver: " + std::string(version::kSolver));
    output.lines.push_back("wall time: " + format_number(outcome.wall_time_ms, 1) + " ms");
    append_shot_report(output.lines, outcome.result);
    if (!outcome.artifacts_dir.empty()) output.lines.push_back("artifacts: " + outcome.artifacts_dir.string());
    return output;
}

JobResult run_sweep(const std::vector<Field>& fields, const CancellationCallback& cancel, const ProgressCallback& progress) {
    cli_workflows::SweepRequest request;
    request.spec_path = field_value(fields, "spec");
    request.out_dir = field_value(fields, "out");

    const cli_workflows::SweepOutcome outcome =
        cli_workflows::run_sweep(request, [&](int completed, int total) {
            progress(completed, total, "completed run " + std::to_string(completed) + " / " + std::to_string(total));
            return !(cancel && cancel());
        });

    JobResult output;
    output.cancelled = outcome.result.cancelled;
    output.lines.push_back("sweep: " + outcome.result.name);
    output.lines.push_back("runs: " + std::to_string(outcome.result.runs.size()) +
                           (outcome.result.cancelled ? " (cancelled)" : ""));
    output.lines.push_back("wall time: " + format_number(outcome.wall_time_ms, 1) + " ms");
    for (const auto& run : outcome.result.runs) {
        std::ostringstream line;
        line << "run " << run.index << " mass " << format_number(units::kg_to_grams(run.summary.beverage_mass_kg))
             << " g, time " << format_number(run.summary.elapsed_time_s) << " s, " << to_string(run.summary.termination);
        output.lines.push_back(line.str());
    }
    if (!outcome.artifacts_dir.empty()) output.lines.push_back("artifacts: " + outcome.artifacts_dir.string());
    return output;
}

JobResult run_calibrate(const std::vector<Field>& fields, const CancellationCallback& cancel, const ProgressCallback& progress) {
    const bool leave_one_out = parse_bool(fields, "leave-one-out");
    const std::vector<std::string> holdout = split_list(field_value(fields, "holdout"));
    if (leave_one_out && !holdout.empty()) {
        throw InputError("holdout", "cannot be combined with leave-one-out=true");
    }
    if (leave_one_out && field_value(fields, "report").empty()) {
        throw InputError("report", "leave-one-out requires a report path");
    }

    cli_workflows::CalibrateRequest request;
    request.shots_dir = field_value(fields, "shots");
    request.coefficients_path = field_value(fields, "coefficients");
    request.fit_names = split_list(field_value(fields, "fit"));
    if (request.fit_names.empty()) throw InputError("fit", "at least one coefficient is required");
    request.holdout_ids = holdout;
    request.leave_one_out = leave_one_out;
    request.max_iterations = parse_int(fields, "max iterations");
    request.report_path = field_value(fields, "report");
    request.out_path = field_value(fields, "out");
    request.id = field_value(fields, "id");
    request.coefficient_version = field_value(fields, "coefficient version");

    progress(0, 0, leave_one_out ? "running leave-one-out validation" : "running calibration fit");
    const cli_workflows::CalibrateOutcome outcome = cli_workflows::run_calibrate(request, cancel);

    JobResult output;
    output.lines.push_back(leave_one_out ? "leave-one-out validation" : "calibration fit");
    if (outcome.leave_one_out_report.has_value()) {
        const auto& validation = *outcome.leave_one_out_report;
        output.lines.push_back(std::string("validation: ") + (validation.passed ? "PASS" : "FAIL"));
        output.lines.push_back("median mass RMSE: " + format_number(validation.median_mass_rmse_g) + " g");
        output.lines.push_back("median time error: " + format_number(validation.median_time_error_s) + " s");
        for (const auto& failure : validation.failed_checks) output.lines.push_back("failed: " + failure);
    }
    if (outcome.report.has_value()) {
        const auto& report = *outcome.report;
        output.lines.push_back("loss: " + format_number(report.starting_loss, 5) + " -> " +
                               format_number(report.final_loss, 5));
        output.lines.push_back("iterations: " + std::to_string(report.iterations) +
                               ", simulations: " + std::to_string(report.simulations));
        for (std::size_t i = 0; i < report.parameters.size(); ++i) {
            output.lines.push_back(report.parameters[i].name + ": " + format_number(report.starting_values[i], 6) +
                                   " -> " + format_number(report.fitted_values[i], 6));
        }
    } else if (outcome.leave_one_out_report.has_value()) {
        output.lines.push_back("full-dataset refit skipped because validation failed");
    }
    if (!outcome.report_written.empty()) output.lines.push_back("report: " + outcome.report_written.string());
    if (!outcome.coefficients_written.empty()) {
        output.lines.push_back("coefficients: " + outcome.coefficients_written.string());
    } else if (outcome.coefficients_withheld_by_validation) {
        output.lines.push_back("coefficients not written: leave-one-out validation failed");
    }
    return output;
}

JobResult run_synthesize(const std::vector<Field>& fields, const CancellationCallback& cancel, const ProgressCallback&) {
    cli_workflows::SynthesizeRequest request;
    request.recipe_path = field_value(fields, "recipe");
    request.coefficients_path = field_value(fields, "coefficients");
    request.noise_g = parse_double(fields, "noise");
    request.seed = static_cast<unsigned int>(parse_int(fields, "seed"));
    request.out_path = field_value(fields, "out");

    const cli_workflows::SynthesizeOutcome outcome = cli_workflows::run_synthesize(request, cancel);
    return JobResult{false,
                     {"wrote synthetic measured shot: " + outcome.out_path.string(),
                      "source hash: " + outcome.result.manifest.result_hash.substr(0, 16),
                      "This is model output, not a real shot."}};
}

JobResult run_bench(const std::vector<Field>& fields, const CancellationCallback& cancel, const ProgressCallback& progress) {
    cli_workflows::BenchRequest request;
    request.seconds = parse_double(fields, "seconds");
    request.repeats = parse_int(fields, "repeats");
    request.recipe_path = field_value(fields, "recipe");
    request.coefficients_path = field_value(fields, "coefficients");

    const cli_workflows::BenchOutcome outcome =
        cli_workflows::run_bench(request, cancel, [&](int completed, int total) {
            progress(completed, total, "completed benchmark " + std::to_string(completed) + " / " + std::to_string(total));
        });

    return JobResult{false,
                     {"repeats: " + std::to_string(request.repeats), "best: " + format_number(outcome.best_ms, 3) + " ms",
                      "median: " + format_number(outcome.median_ms, 3) + " ms",
                      "p95: " + format_number(outcome.p95_ms, 3) + " ms",
                      "throughput: " + format_number(1000.0 / outcome.median_ms, 1) + " simulations/second"}};
}

JobResult run_cfd(const std::vector<Field>& fields, const CancellationCallback& cancel, const ProgressCallback&) {
    const std::string field = field_value(fields, "field");
    if (field != "pressure" && field != "saturation" && field != "temperature" && field != "tds") {
        throw InputError("field", "must be pressure, saturation, temperature, or tds");
    }

    cli_workflows::CfdRequest request;
    request.recipe_path = field_value(fields, "recipe");
    request.coefficients_path = field_value(fields, "coefficients");
    request.radial_cells = parse_int(fields, "radial cells");
    request.axial_cells = parse_int(fields, "axial cells");
    request.dt_s = parse_double(fields, "dt");

    const cli_workflows::CfdOutcome outcome = cli_workflows::run_cfd(request, cancel);

    JobResult output;
    append_cfd_report(output.lines, outcome.result);
    const CfdField* selected = field == "pressure"      ? &outcome.result.pressure_pa
                               : field == "temperature" ? &outcome.result.temperature_k
                               : field == "tds"          ? &outcome.result.pore_tds_fraction
                                                          : &outcome.result.saturation;
    output.lines.push_back(field + " field:");
    for (int j = 0; j < selected->axial_cells(); ++j) {
        std::ostringstream row;
        for (int i = 0; i < selected->radial_cells(); ++i) row << ' ' << format_number(selected->at(i, j), 4);
        output.lines.push_back(row.str());
    }
    return output;
}

JobResult run_cfd3d(const std::vector<Field>& fields, const CancellationCallback& cancel, const ProgressCallback&) {
    cli_workflows::Cfd3dRequest request;
    request.case_path = field_value(fields, "case");
    request.recipe_path = field_value(fields, "recipe");
    request.coefficients_path = field_value(fields, "coefficients");
    request.nx = parse_int(fields, "nx");
    request.ny = parse_int(fields, "ny");
    request.nz = parse_int(fields, "nz");
    request.dt_s = parse_double(fields, "dt");
    request.sample_interval_s = parse_double(fields, "sample interval");
    request.snapshot_interval_s = parse_double(fields, "snapshot interval");
    request.material_path = field_value(fields, "material");
    request.out_dir = field_value(fields, "out");

    const cli_workflows::Cfd3dOutcome outcome = cli_workflows::run_cfd3d(request, cancel);

    JobResult output;
    append_cfd3d_report(output.lines, outcome.result, outcome.snapshots.size());
    if (!outcome.artifacts_dir.empty()) {
        output.lines.push_back("artifacts: " + outcome.artifacts_dir.string());
        if (outcome.manifest.has_value()) output.lines.push_back("result hash: " + outcome.manifest->result_hash);
    }
    return output;
}

void append_grind_report(std::vector<std::string>& lines, const GrinderSpec& spec,
                         const GrinderResult& result) {
    lines.push_back("grinder       " + spec.name);
    lines.push_back("burr gap      " + format_number(spec.burr_gap_um, 1) + " um over " +
                    std::to_string(spec.passes) + " passes");
    lines.push_back("d32 (Sauter)  " + format_number(result.sauter_mean_diameter_um, 2) + " um");
    lines.push_back("spread (sigma) " + format_number(result.geometric_std_dev, 3));
    lines.push_back("fines produced " + format_number(result.cumulative_fines_fraction * 100.0, 3) +
                    " % of mass");
    lines.push_back("bins          " + std::to_string(result.distribution.bins.size()));
    lines.push_back("mass residual " + format_number(result.mass_balance_residual, 6));
    for (const GrindBin& bin : result.distribution.bins) {
        std::ostringstream row;
        row << format_number(units::m_to_microns(bin.diameter_m), 1) << " um : "
            << format_number(bin.mass_fraction, 4);
        lines.push_back(row.str());
    }
    // Mirrors the legacy CLI's `command_grind` usability note (same 150-800 um
    // range a recipe's puck.grind is validated against).
    const ValidationResult usable = result.distribution.validate();
    if (!usable.ok()) {
        lines.push_back("NOTE: this distribution is not usable in a recipe: " + usable.summary());
    } else if (result.sauter_mean_diameter_um < 150.0 || result.sauter_mean_diameter_um > 800.0) {
        lines.push_back("NOTE: d32 is outside the 150-800 um range a recipe supports; widen the burr gap.");
    }
}

JobResult run_grind(const std::vector<Field>& fields, const CancellationCallback&, const ProgressCallback&) {
    cli_workflows::GrindRequest request;
    request.spec_path = field_value(fields, "spec");
    request.out_path = field_value(fields, "out");

    const cli_workflows::GrindOutcome outcome = cli_workflows::run_grind(request);

    JobResult output;
    output.lines.push_back("grinder: " + outcome.spec.name +
                           (request.spec_path.empty() ? " (built-in default spec)" : ""));
    output.lines.push_back("wall time: " + format_number(outcome.wall_time_ms, 1) + " ms");
    append_grind_report(output.lines, outcome.spec, outcome.result);
    if (!outcome.result_path.empty()) {
        output.lines.push_back("artifacts: " + outcome.result_path.parent_path().string());
        output.lines.push_back("  " + outcome.result_path.filename().string() + " (full run)");
        output.lines.push_back("  " + outcome.grind_path.filename().string() +
                               " (paste into a recipe's puck.grind)");
    }
    return output;
}

JobResult run_info(Command command, const CancellationCallback&, const ProgressCallback&) {
    JobResult output;
    if (command == Command::params) {
        output.lines = {"sweepable recipe parameters:"};
        for (const auto& path : cli_workflows::run_params()) output.lines.push_back(path);
    } else if (command == Command::fit_params) {
        output.lines = {"fittable coefficients:"};
        for (const auto& parameter : cli_workflows::run_fit_params()) {
            output.lines.push_back(parameter.name + " [" + format_number(parameter.low, 6) + ", " +
                                   format_number(parameter.high, 6) + "]" +
                                   (parameter.logarithmic ? " (log scale)" : ""));
        }
    } else {
        const auto info = cli_workflows::run_version();
        output.lines = {info.solver + " recipe-schema=" + info.recipe_schema +
                        " result-schema=" + info.result_schema};
    }
    return output;
}

}  // namespace

const std::vector<CommandSpec>& commands() {
    static const std::vector<CommandSpec> value{
        {Command::simulate, "simulate", "Run the deterministic standard shot solver"},
        {Command::sweep, "sweep", "Run a Cartesian recipe parameter sweep"},
        {Command::calibrate, "calibrate", "Fit coefficients against measured shots"},
        {Command::synthesize, "synthesize", "Write a synthetic measured-shot fixture"},
        {Command::bench, "bench", "Benchmark repeated standard simulations"},
        {Command::cfd, "cfd", "Run the experimental 2D axisymmetric CFD solver"},
        {Command::cfd3d, "cfd3d", "Run the experimental 3D Cartesian CFD solver"},
        {Command::grind, "grind", "Model burr geometry as a particle size distribution"},
        {Command::params, "params", "List sweepable recipe parameter paths"},
        {Command::fit_params, "fit-params", "List fittable coefficients and bounds"},
        {Command::version, "version", "Show solver and schema versions"},
    };
    return value;
}

std::string field_value(const std::vector<Field>& fields, const std::string& label) {
    const auto it = std::find_if(fields.begin(), fields.end(),
                                 [&](const Field& field) { return field.label == label; });
    return it == fields.end() ? std::string() : it->value;
}

std::vector<std::string> split_list(const std::string& text) {
    std::vector<std::string> result;
    std::string current;
    for (const char c : text) {
        if (c == ',') {
            if (!current.empty()) result.push_back(current);
            current.clear();
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            current.push_back(c);
        }
    }
    if (!current.empty()) result.push_back(current);
    return result;
}

std::string format_number(double value, int precision) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

double parse_double(const std::vector<Field>& fields, const std::string& label, bool required) {
    const std::string value = field_value(fields, label);
    if (value.empty() && !required) return 0.0;
    if (value.empty()) throw InputError(label, "a value is required");
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(value, &consumed);
        if (consumed != value.size() || !std::isfinite(parsed)) throw std::invalid_argument("not finite");
        return parsed;
    } catch (const std::exception&) {
        throw InputError(label, "must be a finite number");
    }
}

int parse_int(const std::vector<Field>& fields, const std::string& label, bool required) {
    const std::string value = field_value(fields, label);
    if (value.empty() && !required) return 0;
    if (value.empty()) throw InputError(label, "an integer is required");
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != value.size()) throw std::invalid_argument("not an integer");
        return parsed;
    } catch (const std::exception&) {
        throw InputError(label, "must be an integer");
    }
}

bool parse_bool(const std::vector<Field>& fields, const std::string& label) {
    const std::string value = field_value(fields, label);
    if (value.empty() || value == "false" || value == "0" || value == "no") return false;
    if (value == "true" || value == "1" || value == "yes") return true;
    throw InputError(label, "must be true or false");
}

std::vector<Field> default_fields(Command command) {
    switch (command) {
        case Command::simulate:
            return {{"recipe", "assets/recipes/baseline.json"},
                    {"coefficients", "assets/coefficients/default-v1.json"},
                    {"dt", "0.01"},
                    {"sample interval", "0.05"},
                    {"out", ""}};
        case Command::sweep:
            return {{"spec", "assets/sweeps/grind-size.json"}, {"out", ""}};
        case Command::calibrate:
            return {{"shots", "assets/measured_shots"},
                    {"coefficients", ""},
                    {"fit", "kozeny_constant,extraction_rate_ref_s"},
                    {"holdout", ""},
                    {"leave-one-out", "false"},
                    {"report", ""},
                    {"out", ""},
                    {"id", ""},
                    {"coefficient version", "1.0.0"},
                    {"max iterations", "400"}};
        case Command::synthesize:
            return {{"recipe", "assets/recipes/baseline.json"},
                    {"coefficients", ""},
                    {"noise", "0"},
                    {"seed", "1"},
                    {"out", "outputs/measured-shot.json"}};
        case Command::bench:
            return {{"seconds", "60"}, {"repeats", "20"}, {"recipe", "assets/recipes/baseline.json"}, {"coefficients", ""}};
        case Command::cfd:
            return {{"recipe", "assets/recipes/baseline.json"},
                    {"coefficients", ""},
                    {"radial cells", "12"},
                    {"axial cells", "24"},
                    {"dt", "0.005"},
                    {"field", "saturation"}};
        case Command::cfd3d:
            return {{"case", ""},
                    {"recipe", ""},
                    {"coefficients", ""},
                    {"nx", "32"},
                    {"ny", "32"},
                    {"nz", "16"},
                    {"dt", "0.005"},
                    {"sample interval", "0.05"},
                    {"snapshot interval", "1.0"},
                    {"material", ""},
                    {"out", ""}};
        case Command::grind:
            return {{"spec", ""}, {"out", ""}};
        case Command::params:
        case Command::fit_params:
        case Command::version:
            return {};
    }
    return {};
}

JobFunction make_job(Command command, std::vector<Field> fields) {
    return [command, fields = std::move(fields)](const CancellationCallback& cancel, const ProgressCallback& progress) {
        switch (command) {
            case Command::simulate: return run_simulate(fields, cancel, progress);
            case Command::sweep: return run_sweep(fields, cancel, progress);
            case Command::calibrate: return run_calibrate(fields, cancel, progress);
            case Command::synthesize: return run_synthesize(fields, cancel, progress);
            case Command::bench: return run_bench(fields, cancel, progress);
            case Command::cfd: return run_cfd(fields, cancel, progress);
            case Command::cfd3d: return run_cfd3d(fields, cancel, progress);
            case Command::grind: return run_grind(fields, cancel, progress);
            case Command::params:
            case Command::fit_params:
            case Command::version: return run_info(command, cancel, progress);
        }
        return JobResult{};
    };
}

}  // namespace espressolab::tui
