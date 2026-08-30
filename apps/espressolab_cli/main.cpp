#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "espressolab/artifact_io.hpp"
#include "espressolab/cfd.hpp"
#include "espressolab/cfd3d.hpp"
#include "espressolab/cfd3d_artifact_io.hpp"
#include "espressolab/calibration.hpp"
#include "espressolab/experiment.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/units.hpp"
#include "espressolab/version.hpp"
#include "tui/tui.hpp"
#include "workflows.hpp"

namespace {

using namespace espressolab;

constexpr int kOk = 0;
constexpr int kUsageError = 2;
constexpr int kInputError = 3;
constexpr int kSolverFailure = 4;

void print_usage() {
    std::cout << R"(espressolab_cli - deterministic espresso extraction simulator

Usage:
  espressolab_cli simulate --recipe <file> [--coefficients <file>] [--bean <file>]
                          [--out <dir>]
                           [--dt <s>] [--sample-interval <s>] [--quiet]
  espressolab_cli sweep    --spec <file> [--out <dir>] [--quiet]
                           [--workers <n>] [--ring-capacity <n>]

  espressolab_cli calibrate --shots <dir> [--coefficients <file>]
                             --fit <name,name,...> [--holdout <id,id,...>]
                             [--leave-one-out --report <file>]
                            [--out <file>] [--report <file>]
                            [--id <id>] [--coefficient-version <v>]
                            [--max-iterations <n>]
  espressolab_cli synthesize --recipe <file> [--coefficients <file>]
                             [--noise <g>] [--seed <n>] --out <file>

  espressolab_cli bench [--seconds <s>] [--repeats <n>]
  espressolab_cli cfd      --recipe <file> [--coefficients <file>]
                           [--radial <n>] [--axial <n>] [--dt <s>]
                           [--field pressure|saturation|temperature|tds]

  espressolab_cli cfd3d --recipe <file> [--coefficients <file>] [--out <dir>]
                         [--nx <n>] [--ny <n>] [--nz <n>] [--dt <s>]
                         [--sample-interval <s>] [--snapshot-interval <s>]
                         [--material <file>] [--quiet]
  espressolab_cli cfd3d --case <file> [--out <dir>] [--quiet]

  espressolab_cli grind [--spec <file>] [--out <dir>]
                            # burr geometry -> particle size distribution;
                            # outside the shot pipeline, writes its own files

  espressolab_cli params      # sweepable recipe parameters
  espressolab_cli fit-params  # fittable coefficients, with bounds
  espressolab_cli version
  espressolab_cli tui       # interactive terminal UI (POSIX TTY)

Examples:
  espressolab_cli simulate --recipe assets/recipes/baseline.json \
    --coefficients assets/coefficients/default-v1.json --out outputs/shots/baseline
  espressolab_cli sweep --spec assets/sweeps/grind-size.json --out outputs/sweeps/grind-size
  espressolab_cli grind --spec assets/grinders/burr-baseline.json --out outputs/grinds/baseline

  espressolab_cli calibrate --shots assets/measured_shots \
    --fit kozeny_constant,extraction_rate_ref_s --leave-one-out \
    --report outputs/calibration/leave-one-out.json \
    --out assets/coefficients/fitted-v2.json
)";
}

std::map<std::string, std::string> parse_flags(int argc, char** argv, int start) {
    std::map<std::string, std::string> flags;
    for (int i = start; i < argc; ++i) {
        std::string token = argv[i];
        if (token.rfind("--", 0) != 0) continue;
        const std::string key = token.substr(2);
        if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) {
            flags[key] = argv[++i];
        } else {
            flags[key] = "true";  // boolean flag
        }
    }
    return flags;
}

// Audit F10: parse_flags() alone accepts any --name and silently ignores
// positional arguments, so a typo like `--coefficient` (missing the trailing
// `s`) ran with defaults instead of failing. Re-scan the same tokens against
// a per-command allowlist before the command handler trusts them.
bool reject_unknown_options(int argc, char** argv, int start, const std::set<std::string>& allowed,
                             const std::string& command) {
    std::set<std::string> seen;
    for (int i = start; i < argc; ++i) {
        const std::string token = argv[i];
        if (token.rfind("--", 0) != 0) {
            std::cerr << "error UNEXPECTED_ARGUMENT: '" << command
                      << "' does not take positional arguments: '" << token << "'\n";
            return false;
        }
        const std::string key = token.substr(2);
        if (!allowed.count(key)) {
            std::cerr << "error UNKNOWN_OPTION: unrecognized option '--" << key << "' for '" << command << "'\n";
            return false;
        }
        if (!seen.insert(key).second) {
            std::cerr << "error DUPLICATE_OPTION: option '--" << key << "' specified more than once\n";
            return false;
        }
        // Mirror parse_flags(): a token that doesn't start with "--" is this
        // flag's value, not a separate positional argument.
        if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) {
            ++i;
        }
    }
    return true;
}

// Collapses the "reject anything not in `allowed`, then parse what's left"
// dance every command_* handler repeats. Returns nullopt (having already
// printed reject_unknown_options's error) so callers do:
//   const auto parsed = parse_command_flags(argc, argv, {...}, "simulate");
//   if (!parsed) return kUsageError;
//   const auto& flags = *parsed;
std::optional<std::map<std::string, std::string>> parse_command_flags(
    int argc, char** argv, const std::set<std::string>& allowed, const std::string& command) {
    if (!reject_unknown_options(argc, argv, 2, allowed, command)) return std::nullopt;
    return parse_flags(argc, argv, 2);
}

// Audit F3, issue #4: simulate/cfd/cfd3d printed the termination reason but
// always returned kOk, so automation could treat a numerical_failure or
// invalid_state result as a successful run. target_mass_reached and
// time_limit_reached are both successful completions; the remaining three
// reasons (see TerminationReason in result.hpp) are solver-side failures.
// not_terminated (the step budget ran out before any stop condition was met)
// joins the other two here so the CLI's exit code agrees with the REST
// compare handler, which already rejects not_terminated as
// COMPARISON_SIMULATION_FAILED.
bool is_failure_termination(TerminationReason reason) {
    return reason == TerminationReason::numerical_failure ||
           reason == TerminationReason::invalid_state ||
           reason == TerminationReason::not_terminated;
}

// Section 12.2 error shape, printed to stderr so scripts can separate it from
// the artifact paths on stdout.
void print_error(const std::string& code, const std::string& message, const std::string& path) {
    std::cerr << "error " << code;
    if (!path.empty()) std::cerr << " at " << path;
    std::cerr << ": " << message << '\n';
}

void print_shot_report(const ShotResult& result) {
    const ShotSummary& s = result.summary;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  termination     " << to_string(s.termination) << '\n'
              << "  shot time       " << s.elapsed_time_s << " s\n"
              << "  beverage mass   " << units::kg_to_grams(s.beverage_mass_kg) << " g\n"
              << "  brew ratio      1:" << s.brew_ratio << '\n'
              << "  average flow    " << units::m3_s_to_ml_s(s.average_flow_m3_s) << " ml/s\n"
              << "  peak flow       " << units::m3_s_to_ml_s(s.peak_flow_m3_s) << " ml/s\n"
              << "  TDS             " << s.tds_fraction * 100.0 << " %\n"
              << "  extraction      " << s.extraction_yield_fraction * 100.0 << " %\n";
    std::cout << std::scientific << std::setprecision(2)
              << "  mass residuals  water " << result.diagnostics.water_mass_residual_kg
              << " kg, solids " << result.diagnostics.solids_mass_residual_kg << " kg\n"
              << std::defaultfloat;
    std::cout << "  clamps          " << result.diagnostics.clamp_count << '\n'
              << "  samples         " << result.samples.size() << '\n'
              << "  result hash     " << result.manifest.result_hash << '\n';

    // The sensory overlay, only for a run that named a bean. Labelled as an
    // estimate every time it is printed: these axes come from authored priors
    // that have never been checked against tasting (assets/beans/README.md).
    if (result.flavor.has_value()) {
        const FlavorSummary& flavor = result.flavor->summary;
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "\n  sensory estimate (heuristic, uncalibrated) for "
                  << result.flavor->bean_id << '\n'
                  << "    verdict       " << to_string(flavor.verdict) << '\n'
                  << "    match score   " << flavor.match_score << " / 100 vs the bean's target\n"
                  << "    furthest off  " << to_string(flavor.dominant_deviation_axis) << '\n';
        for (std::size_t a = 0; a < kSensoryAxisCount; ++a) {
            const FlavorAxisScore& axis = flavor.axes[a];
            std::cout << "    " << std::left << std::setw(14)
                      << to_string(static_cast<SensoryAxis>(a)) << std::right << std::setw(4)
                      << axis.intensity << "  target " << std::setw(4) << axis.target
                      << "  " << std::showpos << axis.deviation << std::noshowpos << '\n';
        }
        std::cout << std::defaultfloat << std::setprecision(2);
    }

    // FR-08: clamps and invalid states are never silent.
    for (const auto& w : result.warnings) {
        std::cout << "  warning [" << w.code << "] at " << std::fixed << std::setprecision(2)
                  << w.time_s << " s: " << w.message << '\n'
                  << std::defaultfloat;
    }
}

int command_simulate(int argc, char** argv) {
    const auto parsed = parse_command_flags(
        argc, argv, {"recipe", "coefficients", "bean", "out", "dt", "sample-interval", "quiet"}, "simulate");
    if (!parsed) return kUsageError;
    const auto& flags = *parsed;
    if (!flags.count("recipe")) {
        print_error("MISSING_ARGUMENT", "--recipe <file> is required", "");
        return kUsageError;
    }
    const bool quiet = flags.count("quiet") > 0;

    cli_workflows::SimulateRequest request;
    request.recipe_path = flags.at("recipe");
    if (flags.count("coefficients")) request.coefficients_path = flags.at("coefficients");
    if (flags.count("bean")) request.bean_path = flags.at("bean");
    if (flags.count("dt")) request.dt_s = std::stod(flags.at("dt"));
    if (flags.count("sample-interval")) request.sample_interval_s = std::stod(flags.at("sample-interval"));
    if (flags.count("out")) request.out_dir = flags.at("out");

    const cli_workflows::SimulateOutcome outcome = cli_workflows::run_simulate(request);

    if (!quiet) {
        std::cout << "recipe: " << outcome.recipe.name << " (" << flags.at("recipe") << ")\n"
                  << "coefficients: " << outcome.coefficients.id << " v" << outcome.coefficients.version << '\n'
                  << "solver: " << version::kSolver << " dt=" << outcome.config.dt_s
                  << "s sample=" << outcome.config.sample_interval_s << "s\n"
                  << "wall time: " << outcome.wall_time_ms << " ms\n\n";
        print_shot_report(outcome.result);
    }

    if (!outcome.artifacts_dir.empty()) {
        std::cout << "\nartifacts: " << outcome.artifacts_dir.string() << '\n';
    }
    return is_failure_termination(outcome.result.summary.termination) ? kSolverFailure : kOk;
}

int command_sweep(int argc, char** argv) {
    const auto parsed =
        parse_command_flags(argc, argv, {"spec", "out", "workers", "ring-capacity", "quiet"}, "sweep");
    if (!parsed) return kUsageError;
    const auto& flags = *parsed;
    if (!flags.count("spec")) {
        print_error("MISSING_ARGUMENT", "--spec <file> is required", "");
        return kUsageError;
    }
    const bool quiet = flags.count("quiet") > 0;

    cli_workflows::SweepRequest request;
    request.spec_path = flags.at("spec");
    if (flags.count("out")) request.out_dir = flags.at("out");

    // Issue #38: --workers opts into the parallel batch runner;
    // --ring-capacity overrides its fixed worker_count*4 heuristic. The
    // matching ESPRESSOLAB_SWEEP_WORKERS/ESPRESSOLAB_SWEEP_RING_CAPACITY env
    // vars are the same override for benchmarking without editing a command
    // line. A flag wins over its env var; leaving both unset keeps the
    // sequential default exactly as before.
    if (flags.count("workers")) {
        request.workers = static_cast<std::size_t>(std::stoul(flags.at("workers")));
    } else if (const char* env = std::getenv("ESPRESSOLAB_SWEEP_WORKERS")) {
        request.workers = static_cast<std::size_t>(std::stoul(env));
    }
    if (flags.count("ring-capacity")) {
        request.ring_capacity = static_cast<std::size_t>(std::stoul(flags.at("ring-capacity")));
    } else if (const char* env = std::getenv("ESPRESSOLAB_SWEEP_RING_CAPACITY")) {
        request.ring_capacity = static_cast<std::size_t>(std::stoul(env));
    }
    if (request.ring_capacity && !request.workers) {
        print_error("MISSING_ARGUMENT",
                    "--ring-capacity requires --workers (or ESPRESSOLAB_SWEEP_WORKERS) to also be set",
                    "");
        return kUsageError;
    }

    const cli_workflows::SweepOutcome outcome = cli_workflows::run_sweep(request);

    if (!quiet) {
        std::cout << "sweep: " << outcome.result.name << " (" << outcome.result.runs.size() << " runs in "
                  << outcome.wall_time_ms << " ms)\n\n";
        std::cout << artifact_io_sweep::dump_aggregate_csv(outcome.result);
    }

    if (!outcome.artifacts_dir.empty()) {
        std::cout << "\nartifacts: " << outcome.artifacts_dir.string() << '\n';
    }
    return kOk;
}

std::vector<std::string> split_list(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : text) {
        if (c == ',') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            current.push_back(c);
        }
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

int command_synthesize(int argc, char** argv) {
    const auto parsed = parse_command_flags(
        argc, argv, {"recipe", "coefficients", "noise", "seed", "out", "recipe-path"}, "synthesize");
    if (!parsed) return kUsageError;
    const auto& flags = *parsed;
    if (!flags.count("recipe") || !flags.count("out")) {
        print_error("MISSING_ARGUMENT", "--recipe <file> and --out <file> are required", "");
        return kUsageError;
    }

    cli_workflows::SynthesizeRequest request;
    request.recipe_path = flags.at("recipe");
    if (flags.count("coefficients")) request.coefficients_path = flags.at("coefficients");
    request.noise_g = flags.count("noise") ? std::stod(flags.at("noise")) : 0.0;
    request.seed = flags.count("seed") ? static_cast<unsigned int>(std::stoul(flags.at("seed"))) : 1u;
    request.out_path = flags.at("out");
    if (flags.count("recipe-path")) request.recipe_path_for_provenance = flags.at("recipe-path");

    const cli_workflows::SynthesizeOutcome outcome = cli_workflows::run_synthesize(request);

    std::cout << "wrote SYNTHETIC measured shot: " << outcome.out_path.string() << "\n"
              << "  source hash " << outcome.result.manifest.result_hash.substr(0, 16) << "\n"
              << "  " << units::kg_to_grams(outcome.result.summary.beverage_mass_kg) << " g in "
              << outcome.result.summary.elapsed_time_s << " s, noise " << request.noise_g << " g\n\n"
              << "This is model output, not a real shot. Fitting against it validates the\n"
              << "calibration machinery only.\n";
    return kOk;
}

// Section 2.1 performance target: a 60-second shot at 100 Hz in under 20 ms in a
// release build. Also supplies the simulations-per-second number the resume
// bullet template asks for (16.4).
int command_bench(int argc, char** argv) {
    const auto parsed =
        parse_command_flags(argc, argv, {"seconds", "repeats", "recipe", "coefficients"}, "bench");
    if (!parsed) return kUsageError;
    const auto& flags = *parsed;

    cli_workflows::BenchRequest request;
    request.seconds = flags.count("seconds") ? std::stod(flags.at("seconds")) : 60.0;
    request.repeats = flags.count("repeats") ? std::stoi(flags.at("repeats")) : 200;
    request.recipe_path = flags.count("recipe") ? flags.at("recipe") : "assets/recipes/baseline.json";
    if (flags.count("coefficients")) request.coefficients_path = flags.at("coefficients");

    const cli_workflows::BenchOutcome outcome = cli_workflows::run_bench(request);
    const double median = outcome.median_ms;

    std::cout << std::fixed << std::setprecision(3)
              << "shot length     " << request.seconds << " s at " << 1.0 / outcome.dt_s << " Hz ("
              << outcome.steps << " steps)\n"
              << "repeats         " << request.repeats << "\n"
              << "best            " << outcome.best_ms << " ms\n"
              << "median          " << median << " ms\n"
              << "p95             " << outcome.p95_ms << " ms\n"
              << std::setprecision(0)
              << "throughput      " << 1000.0 / median << " simulations/second\n"
              << std::setprecision(1)
              << "budget (2.1)    20 ms -> " << (median < 20.0 ? "PASS" : "FAIL") << ", "
              << 20.0 / median << "x headroom\n"
              << std::defaultfloat;
    return median < 20.0 ? kOk : 1;
}

void print_calibrate_report(bool leave_one_out, const cli_workflows::CalibrateOutcome& outcome) {
    const auto& leave_one_out_report = outcome.leave_one_out_report;
    const auto& report = outcome.report;

    std::cout << (leave_one_out ? "leave-one-out validation with " : "fitting ")
              << outcome.spec.parameters.size() << " parameter(s) against " << outcome.spec.fitting_shots.size()
              << " shot(s)";
    if (!leave_one_out && !outcome.spec.validation_shots.empty()) {
        std::cout << ", holding out " << outcome.spec.validation_shots.size();
    }
    std::cout << "\n\n" << std::defaultfloat;
    if (leave_one_out_report.has_value()) {
        for (const auto& fold : leave_one_out_report->folds) {
            std::cout << "  held out " << fold.shot_id << ": mass RMSE " << fold.loss.mass_rmse_g
                      << " g, time " << fold.loss.time_error_s << " s";
            if (fold.loss.has_tds_measurement) {
                std::cout << ", TDS " << fold.loss.tds_error_percent << " %";
            }
            if (fold.loss.has_pressure_measurement) {
                std::cout << ", pressure RMSE " << fold.loss.pressure_rmse_bar << " bar";
            }
            std::cout << '\n';
        }
        std::cout << "\n  validation " << (leave_one_out_report->passed ? "PASS" : "FAIL")
                  << ": median mass RMSE " << leave_one_out_report->median_mass_rmse_g << " g, "
                  << "median time " << leave_one_out_report->median_time_error_s << " s";
        if (leave_one_out_report->tds_assessed) {
            std::cout << ", median TDS " << *leave_one_out_report->median_tds_error_percent << " %";
        } else {
            std::cout << ", TDS not assessed";
        }
        std::cout << '\n';
        for (const std::string& failure : leave_one_out_report->failed_checks) {
            std::cout << "  failed: " << failure << '\n';
        }
    }
    if (report.has_value()) {
        for (std::size_t i = 0; i < report->parameters.size(); ++i) {
            std::cout << "  " << report->parameters[i].name << ": " << report->starting_values[i]
                      << "  ->  " << report->fitted_values[i] << '\n';
        }
        std::cout << "\n  " << (leave_one_out ? "refit " : "") << "loss "
                  << report->starting_loss << " -> " << report->final_loss << '\n'
                  << "  " << report->iterations << " iterations, " << report->simulations
                  << " simulations, " << outcome.wall_time_ms << " ms"
                  << (report->converged ? " (converged)" : " (hit the iteration limit)") << '\n';
        for (const auto& entry : report->fitting_losses) {
            std::cout << "  fit  " << entry.shot_id << ": mass RMSE " << entry.loss.mass_rmse_g
                      << " g, time " << entry.loss.time_error_s << " s, TDS "
                      << entry.loss.tds_error_percent << " %\n";
        }
    } else {
        std::cout << "\n  full-dataset refit skipped because validation failed\n";
    }
    // Audit-adjacent: `report` is a std::optional and only guaranteed to hold
    // a value in the leave-one-out path when leave_one_out_report->passed
    // (see run_calibrate). Check has_value() explicitly before dereferencing
    // rather than relying on that invariant holding across files.
    if (!leave_one_out_report.has_value() && report.has_value() && report->validation_losses.empty()) {
        std::cout << "\n  WARNING: no held-out validation shot. This fit has not been shown\n"
                   << "           to generalise beyond the shots it was trained on.\n";
    } else if (!leave_one_out_report.has_value() && report.has_value()) {
        for (const auto& entry : report->validation_losses) {
            std::cout << "  held out " << entry.shot_id << ": mass RMSE " << entry.loss.mass_rmse_g
                       << " g, time " << entry.loss.time_error_s << " s, TDS "
                       << entry.loss.tds_error_percent << " %\n";
        }
        std::cout << "  validation loss " << report->validation_loss << '\n';
    }
    if (report.has_value() && report->used_synthetic_data) {
        std::cout << "\n  WARNING: synthetic data. This validates the calibration machinery\n"
                  << "           only; it says nothing about real espresso.\n";
    }

    if (!outcome.report_written.empty()) std::cout << "report: " << outcome.report_written.string() << '\n';
    if (!outcome.coefficients_written.empty()) {
        std::cout << "\ncoefficients: " << outcome.coefficients_written.string() << '\n';
    } else if (outcome.coefficients_withheld_by_validation) {
        std::cout << "\ncoefficients not written: leave-one-out validation failed\n";
    }
}

int command_calibrate(int argc, char** argv) {
    const auto parsed = parse_command_flags(argc, argv,
                                            {"shots", "coefficients", "fit", "holdout", "leave-one-out", "report",
                                             "out", "id", "coefficient-version", "max-iterations"},
                                            "calibrate");
    if (!parsed) return kUsageError;
    const auto& flags = *parsed;
    if (!flags.count("shots") || !flags.count("fit")) {
        print_error("MISSING_ARGUMENT",
                    "--shots <dir> and --fit <name,name,...> are required "
                    "(see `espressolab_cli fit-params`)",
                    "");
        return kUsageError;
    }
    const bool leave_one_out = flags.count("leave-one-out") > 0;
    if (leave_one_out && flags.count("holdout")) {
        print_error("CONFLICTING_ARGUMENTS", "--leave-one-out cannot be combined with --holdout",
                    "leave-one-out");
        return kUsageError;
    }
    if (leave_one_out && !flags.count("report")) {
        print_error("MISSING_ARGUMENT", "--leave-one-out requires --report <file>", "report");
        return kUsageError;
    }

    cli_workflows::CalibrateRequest request;
    request.shots_dir = flags.at("shots");
    if (flags.count("coefficients")) request.coefficients_path = flags.at("coefficients");
    if (flags.count("max-iterations")) request.max_iterations = std::stoi(flags.at("max-iterations"));
    request.fit_names = split_list(flags.at("fit"));
    request.holdout_ids = flags.count("holdout") ? split_list(flags.at("holdout")) : std::vector<std::string>{};
    request.leave_one_out = leave_one_out;
    if (flags.count("report")) request.report_path = flags.at("report");
    if (flags.count("out")) request.out_path = flags.at("out");
    if (flags.count("id")) request.id = flags.at("id");
    request.coefficient_version = flags.count("coefficient-version") ? flags.at("coefficient-version") : "1.0.0";
    if (request.out_path.empty()) {
        // Legacy `dump_fitted_coefficients_json` names the run after the
        // output file's stem; keep that even though the workflow layer
        // cannot derive a stem from an empty path.
    } else if (request.id.empty()) {
        request.id = std::filesystem::path(request.out_path).stem().string();
    }

    const cli_workflows::CalibrateOutcome outcome = cli_workflows::run_calibrate(request);
    print_calibrate_report(leave_one_out, outcome);
    return outcome.leave_one_out_report.has_value() && !outcome.leave_one_out_report->passed ? 1 : kOk;
}

int command_cfd(int argc, char** argv) {
    const auto parsed = parse_command_flags(
        argc, argv, {"recipe", "coefficients", "radial", "axial", "dt", "field"}, "cfd");
    if (!parsed) return kUsageError;
    const auto& flags = *parsed;
    if (!flags.count("recipe")) {
        print_error("MISSING_ARGUMENT", "--recipe <file> is required", "");
        return kUsageError;
    }
    // Audit F12: validate --field before running the solver so an unknown
    // name (e.g. a typo) fails loudly instead of silently falling through
    // to the saturation field.
    static const std::set<std::string> kFieldNames = {"pressure", "saturation", "temperature", "tds"};
    if (flags.count("field") && !kFieldNames.count(flags.at("field"))) {
        print_error("UNKNOWN_OPTION", "unrecognized --field '" + flags.at("field") + "' (expected pressure, "
                    "saturation, temperature, or tds)", "field");
        return kUsageError;
    }

    cli_workflows::CfdRequest request;
    request.recipe_path = flags.at("recipe");
    if (flags.count("coefficients")) request.coefficients_path = flags.at("coefficients");
    if (flags.count("radial")) request.radial_cells = std::stoi(flags.at("radial"));
    if (flags.count("axial")) request.axial_cells = std::stoi(flags.at("axial"));
    if (flags.count("dt")) request.dt_s = std::stod(flags.at("dt"));

    const cli_workflows::CfdOutcome outcome = cli_workflows::run_cfd(request);
    const CfdResult& result = outcome.result;

    std::cout << "recipe: " << outcome.recipe.name << " (" << flags.at("recipe") << ")\n"
              << "solver: " << result.solver_version << " CFD mesh "
              << outcome.config.mesh.radial_cells << " x " << outcome.config.mesh.axial_cells
              << " (r x z), dt=" << outcome.config.dt_s << "s\n"
              << "wall time: " << outcome.wall_time_ms << " ms\n\n";

    std::cout << std::fixed;
    std::cout << "  termination     " << to_string(result.termination) << '\n'
              << std::setprecision(2)
              << "  shot time       " << result.elapsed_time_s << " s\n"
              << "  beverage mass   " << units::kg_to_grams(result.beverage_mass_kg) << " g\n"
              << "  TDS             " << result.tds_fraction * 100.0 << " %\n"
              << "  extraction      " << result.extraction_yield_fraction * 100.0 << " %\n";

    const CfdDiagnostics& d = result.diagnostics;
    std::cout << std::scientific << std::setprecision(3)
              << "\n  -- verification --\n"
              << "  max |div u_t|   " << d.max_total_velocity_divergence_1_s << " 1/s\n"
              << "  pressure resid  " << d.pressure_residual << '\n'
              << "  water residual  " << d.water_mass_residual_kg << " kg\n"
              << "  solids residual " << d.solids_mass_residual_kg << " kg\n"
              << std::defaultfloat
              << "  pressure iters  " << d.pressure_iterations_total << " over "
              << d.step_count << " steps\n"
              << "  max Courant     " << d.max_courant_number << '\n'
              << "  saturation clamps " << d.saturation_clamp_count << '\n';

    if (flags.count("field")) {
        const std::string which = flags.at("field");
        const CfdField& field = which == "pressure"    ? result.pressure_pa
                                : which == "temperature" ? result.temperature_k
                                : which == "tds"         ? result.pore_tds_fraction
                                                         : result.saturation;
        std::cout << "\n  " << which << " field (rows = depth, columns = radius)\n";
        std::cout << std::fixed << std::setprecision(4);
        for (int j = 0; j < field.axial_cells(); ++j) {
            std::cout << "   ";
            for (int i = 0; i < field.radial_cells(); ++i) {
                std::cout << std::setw(10) << field.at(i, j);
            }
            std::cout << '\n';
        }
    }
    return is_failure_termination(result.termination) ? kSolverFailure : kOk;
}

int command_grind(int argc, char** argv) {
    const auto parsed = parse_command_flags(argc, argv, {"spec", "out"}, "grind");
    if (!parsed) return kUsageError;
    const auto& flags = *parsed;

    cli_workflows::GrindRequest request;
    if (flags.count("spec")) request.spec_path = flags.at("spec");
    if (flags.count("out")) request.out_path = flags.at("out");

    const cli_workflows::GrindOutcome outcome = cli_workflows::run_grind(request);
    const GrinderResult& result = outcome.result;

    std::cout << "grinder: " << outcome.spec.name
              << (request.spec_path.empty() ? " (built-in default spec)"
                                            : " (" + request.spec_path + ")")
              << "\nburr gap: " << outcome.spec.burr_gap_um << " um over " << outcome.spec.passes
              << " passes\nwall time: " << outcome.wall_time_ms << " ms\n\n";

    std::cout << std::fixed << std::setprecision(2)
              << "  d32 (Sauter)    " << result.sauter_mean_diameter_um << " um\n"
              << std::setprecision(3)
              << "  spread (sigma)  " << result.geometric_std_dev << '\n'
              << "  fines produced  " << result.cumulative_fines_fraction * 100.0 << " % of mass\n"
              << "  bins            " << result.distribution.bins.size() << '\n'
              << std::scientific << std::setprecision(2)
              << "  mass residual   " << result.mass_balance_residual << '\n'
              << std::defaultfloat;

    std::cout << "\n  distribution (diameter um : mass fraction)\n";
    std::cout << std::fixed;
    for (const GrindBin& bin : result.distribution.bins) {
        const double um = units::m_to_microns(bin.diameter_m);
        std::cout << "   " << std::setw(8) << std::setprecision(1) << um << " : "
                  << std::setw(6) << std::setprecision(4) << bin.mass_fraction << "  "
                  << std::string(static_cast<std::size_t>(bin.mass_fraction * 200.0), '#') << '\n';
    }

    // A grinder spec is free to describe a bed the shot correlations do not
    // cover, so say plainly whether this distribution can be used as-is.
    const ValidationResult usable = result.distribution.validate();
    if (!usable.ok()) {
        std::cout << "\n  NOTE: this distribution is not usable in a recipe: " << usable.summary()
                  << '\n';
    } else if (result.sauter_mean_diameter_um < 150.0 ||
               result.sauter_mean_diameter_um > 800.0) {
        std::cout << "\n  NOTE: d32 is outside the 150-800 um range a recipe supports, so a "
                     "recipe carrying this distribution will be rejected. Widen the burr gap.\n";
    }

    if (!outcome.result_path.empty()) {
        std::cout << "\nartifacts: " << outcome.result_path.parent_path().string() << '\n'
                  << "  " << outcome.result_path.filename().string() << "  (full run)\n"
                  << "  " << outcome.grind_path.filename().string()
                  << "  (paste into a recipe's puck.grind)\n";
    }
    return kOk;
}

int command_cfd3d(int argc, char** argv) {
    const auto parsed = parse_command_flags(argc, argv,
                                            {"recipe", "case", "coefficients", "nx", "ny", "nz", "dt",
                                             "sample-interval", "snapshot-interval", "material", "out", "quiet"},
                                            "cfd3d");
    if (!parsed) return kUsageError;
    const auto& flags = *parsed;
    if (!flags.count("recipe") && !flags.count("case")) {
        print_error("MISSING_ARGUMENT", "--recipe <file> or --case <file> is required", "");
        return kUsageError;
    }
    const bool quiet = flags.count("quiet") > 0;

    cli_workflows::Cfd3dRequest request;
    if (flags.count("case")) request.case_path = flags.at("case");
    if (flags.count("recipe")) request.recipe_path = flags.at("recipe");
    if (flags.count("coefficients")) request.coefficients_path = flags.at("coefficients");
    if (flags.count("nx")) request.nx = std::stoi(flags.at("nx"));
    if (flags.count("ny")) request.ny = std::stoi(flags.at("ny"));
    if (flags.count("nz")) request.nz = std::stoi(flags.at("nz"));
    if (flags.count("dt")) request.dt_s = std::stod(flags.at("dt"));
    if (flags.count("sample-interval")) request.sample_interval_s = std::stod(flags.at("sample-interval"));
    if (flags.count("snapshot-interval")) request.snapshot_interval_s = std::stod(flags.at("snapshot-interval"));
    if (flags.count("material")) request.material_path = flags.at("material");
    if (flags.count("out")) request.out_dir = flags.at("out");

    const cli_workflows::Cfd3dOutcome outcome = cli_workflows::run_cfd3d(request);
    const Cfd3dResult& result = outcome.result;
    const Cfd3dConfig& config = outcome.cfd3d_case.config;

    if (!quiet) {
        std::cout << "recipe: " << outcome.cfd3d_case.recipe.name << " (3D Cartesian CFD)\n"
                  << "solver: " << result.solver_version << " mesh " << config.mesh.nx << " x "
                  << config.mesh.ny << " x " << config.mesh.nz << " (x x y x z), dt="
                  << config.dt_s << "s snapshots=" << outcome.snapshots.size() << "\n"
                  << "wall time: " << outcome.wall_time_ms << " ms\n\n"
                  << std::fixed << std::setprecision(2)
                  << "  termination     " << to_string(result.termination) << '\n'
                  << "  shot time       " << result.elapsed_time_s << " s\n"
                  << "  beverage mass   " << units::kg_to_grams(result.beverage_mass_kg) << " g\n"
                  << "  TDS             " << result.tds_fraction * 100.0 << " %\n"
                  << "  extraction      " << result.extraction_yield_fraction * 100.0 << " %\n"
                  << std::scientific << std::setprecision(3)
                  << "  water residual  " << result.diagnostics.water_mass_residual_kg << " kg\n"
                  << "  solids residual " << result.diagnostics.solids_mass_residual_kg << " kg\n"
                  << "  energy residual " << result.diagnostics.energy_residual_j << " J\n"
                  << "  pressure resid  " << result.diagnostics.pressure_residual << '\n'
                  << std::defaultfloat;
    }
    if (!outcome.artifacts_dir.empty()) {
        std::cout << "\nartifacts: " << outcome.artifacts_dir.string() << '\n'
                  << "result hash: " << outcome.manifest->result_hash << '\n';
    }
    return is_failure_termination(result.termination) ? kSolverFailure : kOk;
}

int command_params(int, char**) {
    for (const auto& path : cli_workflows::run_params()) std::cout << path << '\n';
    return kOk;
}

int command_fit_params(int, char**) {
    for (const auto& parameter : cli_workflows::run_fit_params()) {
        std::cout << parameter.name << "  [" << parameter.low << ", " << parameter.high << "]"
                  << (parameter.logarithmic ? "  (log scale)" : "") << '\n';
    }
    return kOk;
}

int command_version(int, char**) {
    const auto info = cli_workflows::run_version();
    std::cout << info.solver << " recipe-schema=" << info.recipe_schema
              << " result-schema=" << info.result_schema << '\n';
    return kOk;
}

using CommandHandler = int (*)(int, char**);

// Every command whose handler shares the command_*(argc, argv) shape, keyed
// by its argv[1] name. `tui` and the help aliases are dispatched separately
// in main() below: run_tui() takes no arguments, and the help aliases are not
// really commands so much as print_usage() triggers.
const std::map<std::string, CommandHandler>& command_table() {
    static const std::map<std::string, CommandHandler> table{
        {"simulate", command_simulate},   {"sweep", command_sweep},
        {"bench", command_bench},         {"cfd", command_cfd},
        {"grind", command_grind},         {"cfd3d", command_cfd3d},
        {"calibrate", command_calibrate}, {"synthesize", command_synthesize},
        {"params", command_params},       {"fit-params", command_fit_params},
        {"version", command_version},
    };
    return table;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return kUsageError;
    }
    const std::string command = argv[1];

    try {
        const auto& table = command_table();
        const auto it = table.find(command);
        if (it != table.end()) return it->second(argc, argv);
        if (command == "tui") return run_tui();
        if (command == "--help" || command == "-h" || command == "help") {
            print_usage();
            return kOk;
        }
    } catch (const espressolab::artifact_io::LoadError& e) {
        print_error(e.code, e.what(), e.path);
        return kInputError;
    } catch (const espressolab::InvalidInputError& e) {
        for (const auto& issue : e.validation().issues()) {
            print_error(issue.code, issue.message, issue.path);
        }
        return kInputError;
    } catch (const std::exception& e) {
        print_error("INTERNAL_ERROR", e.what(), "");
        return 1;
    }

    print_error("UNKNOWN_COMMAND", "unknown command '" + command + "'", "");
    print_usage();
    return kUsageError;
}
