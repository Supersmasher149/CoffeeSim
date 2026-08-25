#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>

#include "espressolab/artifact_io.hpp"
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
  espressolab_cli params
  espressolab_cli version

Examples:
  espressolab_cli simulate --recipe assets/recipes/baseline.json \
    --coefficients assets/coefficients/default-v1.json --out outputs/shots/baseline
  espressolab_cli sweep --spec assets/sweeps/grind-size.json --out outputs/sweeps/grind-size
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
