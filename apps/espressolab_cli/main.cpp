#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "espressolab/artifact_io.hpp"
#include "espressolab/calibration.hpp"
#include "espressolab/experiment.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/units.hpp"
#include "espressolab/version.hpp"

namespace {

using namespace espressolab;

constexpr int kOk = 0;
constexpr int kUsageError = 2;
constexpr int kInputError = 3;

void print_usage() {
    std::cout << R"(espressolab_cli - deterministic espresso extraction simulator

Usage:
  espressolab_cli simulate --recipe <file> [--coefficients <file>] [--out <dir>]
                           [--dt <s>] [--sample-interval <s>] [--quiet]
  espressolab_cli sweep    --spec <file> [--out <dir>] [--quiet]

  espressolab_cli calibrate --shots <dir> [--coefficients <file>]
                            --fit <name,name,...> [--holdout <id,id,...>]
                            [--out <file>] [--report <file>]
                            [--id <id>] [--coefficient-version <v>]
                            [--max-iterations <n>]
  espressolab_cli synthesize --recipe <file> [--coefficients <file>]
                             [--noise <g>] [--seed <n>] --out <file>

  espressolab_cli bench [--seconds <s>] [--repeats <n>]

  espressolab_cli params      # sweepable recipe parameters
  espressolab_cli fit-params  # fittable coefficients, with bounds
  espressolab_cli version

Examples:
  espressolab_cli simulate --recipe assets/recipes/baseline.json \
    --coefficients assets/coefficients/default-v1.json --out outputs/shots/baseline
  espressolab_cli sweep --spec assets/sweeps/grind-size.json --out outputs/sweeps/grind-size

  espressolab_cli calibrate --shots assets/measured_shots \
    --fit kozeny_constant,extraction_rate_ref_s --holdout shot-3 \
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

    // FR-08: clamps and invalid states are never silent.
    for (const auto& w : result.warnings) {
        std::cout << "  warning [" << w.code << "] at " << std::fixed << std::setprecision(2)
                  << w.time_s << " s: " << w.message << '\n'
                  << std::defaultfloat;
    }
}

int command_simulate(int argc, char** argv) {
    const auto flags = parse_flags(argc, argv, 2);
    if (!flags.count("recipe")) {
        print_error("MISSING_ARGUMENT", "--recipe <file> is required", "");
        return kUsageError;
    }
    const bool quiet = flags.count("quiet") > 0;

    Recipe recipe = artifact_io::load_recipe_file(flags.at("recipe"));
    ModelCoefficients coefficients;
    if (flags.count("coefficients")) {
        coefficients = artifact_io::load_coefficients_file(flags.at("coefficients"));
    }

    SimulationConfig config;
    if (flags.count("dt")) config.dt_s = std::stod(flags.at("dt"));
    if (flags.count("sample-interval")) {
        config.sample_interval_s = std::stod(flags.at("sample-interval"));
    }

    const Simulator simulator;
    const auto started = std::chrono::steady_clock::now();
    ShotResult result = simulator.run(recipe, coefficients, config);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    artifact_io::stamp_manifest(result, recipe, coefficients, config);

    if (!quiet) {
        std::cout << "recipe: " << recipe.name << " (" << flags.at("recipe") << ")\n"
                  << "coefficients: " << coefficients.id << " v" << coefficients.version << '\n'
                  << "solver: " << version::kSolver << " dt=" << config.dt_s
                  << "s sample=" << config.sample_interval_s << "s\n"
                  << "wall time: "
                  << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / 1000.0
                  << " ms\n\n";
        print_shot_report(result);
    }

    if (flags.count("out")) {
        const std::filesystem::path directory = flags.at("out");
        artifact_io::write_shot_artifacts(directory, recipe, coefficients, result);
        std::cout << "\nartifacts: " << std::filesystem::absolute(directory).string() << '\n';
    }
    return kOk;
}

int command_sweep(int argc, char** argv) {
    const auto flags = parse_flags(argc, argv, 2);
    if (!flags.count("spec")) {
        print_error("MISSING_ARGUMENT", "--spec <file> is required", "");
        return kUsageError;
    }
    const bool quiet = flags.count("quiet") > 0;

    SweepSpec spec = artifact_io_sweep::load_sweep_spec_file(flags.at("spec"));
    const ExperimentRunner runner;

    const auto started = std::chrono::steady_clock::now();
    SweepResult result = runner.run(spec);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    if (!quiet) {
        std::cout << "sweep: " << result.name << " (" << result.runs.size() << " runs in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << " ms)\n\n";
        std::cout << artifact_io_sweep::dump_aggregate_csv(result);
    }

    if (flags.count("out")) {
        const std::filesystem::path directory = flags.at("out");
        artifact_io_sweep::write_sweep_artifacts(directory, result);
        std::cout << "\nartifacts: " << std::filesystem::absolute(directory).string() << '\n';
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
    const auto flags = parse_flags(argc, argv, 2);
    if (!flags.count("recipe") || !flags.count("out")) {
        print_error("MISSING_ARGUMENT", "--recipe <file> and --out <file> are required", "");
        return kUsageError;
    }

    const Recipe recipe = artifact_io::load_recipe_file(flags.at("recipe"));
    ModelCoefficients coefficients;
    if (flags.count("coefficients")) {
        coefficients = artifact_io::load_coefficients_file(flags.at("coefficients"));
    }
    const double noise_g = flags.count("noise") ? std::stod(flags.at("noise")) : 0.0;
    const unsigned int seed =
        flags.count("seed") ? static_cast<unsigned int>(std::stoul(flags.at("seed"))) : 1u;

    SimulationConfig config;
    ShotResult result = Simulator().run(recipe, coefficients, config);
    artifact_io::stamp_manifest(result, recipe, coefficients, config);

    const std::filesystem::path out = flags.at("out");
    if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path());
    std::ofstream stream(out, std::ios::trunc);
    if (!stream) {
        print_error("WRITE_FAILED", "could not write " + out.string(), out.string());
        return 1;
    }
    stream << calibration::io::dump_synthetic_shot_json(
        recipe, result, flags.count("recipe-path") ? flags.at("recipe-path") : std::string(),
        noise_g, seed);

    std::cout << "wrote SYNTHETIC measured shot: " << std::filesystem::absolute(out).string()
              << "\n"
              << "  source hash " << result.manifest.result_hash.substr(0, 16) << "\n"
              << "  " << units::kg_to_grams(result.summary.beverage_mass_kg) << " g in "
              << result.summary.elapsed_time_s << " s, noise " << noise_g << " g\n\n"
              << "This is model output, not a real shot. Fitting against it validates the\n"
              << "calibration machinery only.\n";
    return kOk;
}

// Section 2.1 performance target: a 60-second shot at 100 Hz in under 20 ms in a
// release build. Also supplies the simulations-per-second number the resume
// bullet template asks for (16.4).
int command_bench(int argc, char** argv) {
    const auto flags = parse_flags(argc, argv, 2);
    const double seconds = flags.count("seconds") ? std::stod(flags.at("seconds")) : 60.0;
    const int repeats = flags.count("repeats") ? std::stoi(flags.at("repeats")) : 200;

    Recipe recipe = artifact_io::load_recipe_file(
        flags.count("recipe") ? flags.at("recipe") : "assets/recipes/baseline.json");
    recipe.maximum_time_s = seconds;
    recipe.target_beverage_mass_kg.reset();  // always run the full duration

    ModelCoefficients coefficients;
    if (flags.count("coefficients")) {
        coefficients = artifact_io::load_coefficients_file(flags.at("coefficients"));
    }

    const SimulationConfig config;
    const Simulator simulator;

    // One untimed run so the first-touch page faults do not land in the sample.
    ShotResult warmup = simulator.run(recipe, coefficients, config);
    long long steps = warmup.diagnostics.step_count;

    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(repeats));
    for (int i = 0; i < repeats; ++i) {
        const auto started = std::chrono::steady_clock::now();
        const ShotResult result = simulator.run(recipe, coefficients, config);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        samples.push_back(
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
            1.0e6);
        steps = result.diagnostics.step_count;
    }

    std::sort(samples.begin(), samples.end());
    const double median = samples[samples.size() / 2];
    const double best = samples.front();
    const double p95 = samples[static_cast<std::size_t>(0.95 * (samples.size() - 1))];

    std::cout << std::fixed << std::setprecision(3)
              << "shot length     " << seconds << " s at " << 1.0 / config.dt_s << " Hz ("
              << steps << " steps)\n"
              << "repeats         " << repeats << "\n"
              << "best            " << best << " ms\n"
              << "median          " << median << " ms\n"
              << "p95             " << p95 << " ms\n"
              << std::setprecision(0)
              << "throughput      " << 1000.0 / median << " simulations/second\n"
              << std::setprecision(1)
              << "budget (2.1)    20 ms -> " << (median < 20.0 ? "PASS" : "FAIL") << ", "
              << 20.0 / median << "x headroom\n"
              << std::defaultfloat;
    return median < 20.0 ? kOk : 1;
}

int command_calibrate(int argc, char** argv) {
    const auto flags = parse_flags(argc, argv, 2);
    if (!flags.count("shots") || !flags.count("fit")) {
        print_error("MISSING_ARGUMENT",
                    "--shots <dir> and --fit <name,name,...> are required "
                    "(see `espressolab_cli fit-params`)",
                    "");
        return kUsageError;
    }

    calibration::CalibrationSpec spec;
    if (flags.count("coefficients")) {
        spec.starting_point = artifact_io::load_coefficients_file(flags.at("coefficients"));
    }
    if (flags.count("max-iterations")) {
        spec.maximum_iterations = std::stoi(flags.at("max-iterations"));
    }

    for (const std::string& name : split_list(flags.at("fit"))) {
        const auto parameter = calibration::tunable_parameter(name);
        if (!parameter.has_value()) {
            print_error("UNKNOWN_PARAMETER_NAME",
                        "'" + name + "' is not a fittable coefficient; "
                        "run `espressolab_cli fit-params` for the list",
                        name);
            return kInputError;
        }
        spec.parameters.push_back(*parameter);
    }

    const std::vector<std::string> holdout =
        flags.count("holdout") ? split_list(flags.at("holdout")) : std::vector<std::string>{};
    std::vector<std::string> fitting_ids;
    std::vector<std::string> validation_ids;

    std::vector<std::string> unmatched_holdout = holdout;
    for (auto& shot : calibration::io::load_measured_shot_directory(flags.at("shots"))) {
        const auto matches = [&](const std::string& name) {
            return name == shot.id || name == shot.source_stem;
        };
        const bool held_out = std::any_of(holdout.begin(), holdout.end(), matches);
        std::erase_if(unmatched_holdout, matches);
        if (held_out) {
            validation_ids.push_back(shot.id);
            spec.validation_shots.push_back(std::move(shot));
        } else {
            fitting_ids.push_back(shot.id);
            spec.fitting_shots.push_back(std::move(shot));
        }
    }

    // A holdout that silently matches nothing would report a fit as validated
    // when it never was, which is worse than refusing to run.
    if (!unmatched_holdout.empty()) {
        for (const std::string& name : unmatched_holdout) {
            print_error("UNKNOWN_HOLDOUT_SHOT",
                        "no measured shot with id or filename '" + name + "' in " +
                            flags.at("shots"),
                        "holdout");
        }
        return kInputError;
    }

    if (spec.fitting_shots.empty()) {
        print_error("NO_FITTING_SHOTS",
                    "no measured shots to fit in " + flags.at("shots") +
                        " (every shot found was held out)",
                    "shots");
        return kInputError;
    }

    std::cout << "fitting " << spec.parameters.size() << " parameter(s) against "
              << spec.fitting_shots.size() << " shot(s)";
    if (!spec.validation_shots.empty()) {
        std::cout << ", holding out " << spec.validation_shots.size();
    }
    std::cout << "\n\n";

    const auto started = std::chrono::steady_clock::now();
    const calibration::CalibrationReport report = calibration::fit(spec);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    std::cout << std::defaultfloat;
    for (std::size_t i = 0; i < report.parameters.size(); ++i) {
        std::cout << "  " << report.parameters[i].name << ": " << report.starting_values[i]
                  << "  ->  " << report.fitted_values[i] << '\n';
    }
    std::cout << "\n  loss " << report.starting_loss << " -> " << report.final_loss << '\n'
              << "  " << report.iterations << " iterations, " << report.simulations
              << " simulations, "
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms"
              << (report.converged ? " (converged)" : " (hit the iteration limit)") << '\n';

    for (const auto& entry : report.fitting_losses) {
        std::cout << "  fit  " << entry.shot_id << ": mass RMSE " << entry.loss.mass_rmse_g
                  << " g, time " << entry.loss.time_error_s << " s, TDS "
                  << entry.loss.tds_error_percent << " %\n";
    }
    if (report.validation_losses.empty()) {
        std::cout << "\n  WARNING: no held-out validation shot. This fit has not been shown\n"
                  << "           to generalise beyond the shots it was trained on.\n";
    } else {
        for (const auto& entry : report.validation_losses) {
            std::cout << "  held out " << entry.shot_id << ": mass RMSE " << entry.loss.mass_rmse_g
                      << " g, time " << entry.loss.time_error_s << " s, TDS "
                      << entry.loss.tds_error_percent << " %\n";
        }
        std::cout << "  validation loss " << report.validation_loss << '\n';
    }
    if (report.used_synthetic_data) {
        std::cout << "\n  WARNING: synthetic data. This validates the calibration machinery\n"
                  << "           only; it says nothing about real espresso.\n";
    }

    const auto write = [](const std::filesystem::path& path, const std::string& contents) {
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::trunc);
        if (!stream) throw std::runtime_error("could not write " + path.string());
        stream << contents;
    };

    if (flags.count("out")) {
        const std::filesystem::path out = flags.at("out");
        write(out, calibration::io::dump_fitted_coefficients_json(
                       report, flags.count("id") ? flags.at("id") : out.stem().string(),
                       flags.count("coefficient-version") ? flags.at("coefficient-version")
                                                          : "1.0.0",
                       fitting_ids, validation_ids));
        std::cout << "\ncoefficients: " << std::filesystem::absolute(out).string() << '\n';
    }
    if (flags.count("report")) {
        const std::filesystem::path path = flags.at("report");
        write(path, calibration::io::dump_report_json(report));
        std::cout << "report: " << std::filesystem::absolute(path).string() << '\n';
    }
    return kOk;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return kUsageError;
    }
    const std::string command = argv[1];

    try {
        if (command == "simulate") return command_simulate(argc, argv);
        if (command == "sweep") return command_sweep(argc, argv);
        if (command == "bench") return command_bench(argc, argv);
        if (command == "calibrate") return command_calibrate(argc, argv);
        if (command == "synthesize") return command_synthesize(argc, argv);
        if (command == "fit-params") {
            for (const auto& name : espressolab::calibration::tunable_parameter_names()) {
                const auto parameter = espressolab::calibration::tunable_parameter(name);
                std::cout << name << "  [" << parameter->low << ", " << parameter->high << "]"
                          << (parameter->logarithmic ? "  (log scale)" : "") << '\n';
            }
            return kOk;
        }
        if (command == "params") {
            for (const auto& path : espressolab::supported_parameter_paths()) {
                std::cout << path << '\n';
            }
            return kOk;
        }
        if (command == "version") {
            std::cout << espressolab::version::kSolver
                      << " recipe-schema=" << espressolab::version::kRecipeSchema
                      << " result-schema=" << espressolab::version::kResultSchema << '\n';
            return kOk;
        }
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
