#include "workflows.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace espressolab::cli_workflows {

namespace {

double elapsed_ms(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
        .count();
}

void throw_single_issue(const std::string& code, const std::string& message,
                        const std::string& path) {
    ValidationResult validation;
    validation.add(code, message, path);
    throw InvalidInputError(validation);
}

}  // namespace

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw artifact_io::LoadError("FILE_NOT_FOUND", path.string(), "could not open " + path.string());
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

void write_text_file(const std::filesystem::path& path, const std::string& contents) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::trunc);
    if (!stream) throw std::runtime_error("could not write " + path.string());
    stream << contents;
}

Cfd3dMaterialField load_cfd3d_material(const std::filesystem::path& path, const Cfd3dMesh& mesh) {
    using nlohmann::json;
    json root;
    try {
        root = json::parse(read_text_file(path));
    } catch (const json::parse_error& error) {
        throw artifact_io::LoadError("MALFORMED_JSON", path.string(), error.what());
    }
    if (root.is_number()) return Cfd3dMaterialField(mesh.nx, mesh.ny, mesh.nz, root.get<double>());
    if (root.is_array()) root = json{{"values", std::move(root)}};
    if (!root.is_object()) {
        throw artifact_io::LoadError("MALFORMED_JSON", path.string(),
                                     "material must be a number, array, or object");
    }
    const std::size_t expected =
        static_cast<std::size_t>(mesh.nx) * static_cast<std::size_t>(mesh.ny) * static_cast<std::size_t>(mesh.nz);
    if (root.contains("uniform")) {
        if (!root.at("uniform").is_number()) {
            throw artifact_io::LoadError("MALFORMED_JSON", path.string(), "material uniform must be a number");
        }
        return Cfd3dMaterialField(mesh.nx, mesh.ny, mesh.nz, root.at("uniform").get<double>());
    }
    if (!root.contains("values") || !root.at("values").is_array() || root.at("values").size() != expected) {
        throw artifact_io::LoadError("OUT_OF_RANGE", path.string(),
                                     "material values must match mesh dimensions");
    }
    Cfd3dMaterialField material(mesh.nx, mesh.ny, mesh.nz, 1.0);
    const std::size_t plane = static_cast<std::size_t>(mesh.nx) * static_cast<std::size_t>(mesh.ny);
    for (std::size_t index = 0; index < expected; ++index) {
        if (!root.at("values")[index].is_number()) {
            throw artifact_io::LoadError("MALFORMED_JSON", path.string(), "material values must be numbers");
        }
        const int x = static_cast<int>(index % static_cast<std::size_t>(mesh.nx));
        const int y = static_cast<int>((index / static_cast<std::size_t>(mesh.nx)) % static_cast<std::size_t>(mesh.ny));
        const int z = static_cast<int>(index / plane);
        material.at(x, y, z) = root.at("values")[index].get<double>();
    }
    return material;
}

// ---------------------------------------------------------------- simulate --
SimulateOutcome run_simulate(const SimulateRequest& request, const CancellationCallback& is_cancelled) {
    SimulateOutcome outcome;
    outcome.recipe = artifact_io::load_recipe_file(request.recipe_path);
    if (!request.coefficients_path.empty()) {
        outcome.coefficients = artifact_io::load_coefficients_file(request.coefficients_path);
    }
    if (request.dt_s.has_value()) outcome.config.dt_s = *request.dt_s;
    if (request.sample_interval_s.has_value()) outcome.config.sample_interval_s = *request.sample_interval_s;

    const auto started = std::chrono::steady_clock::now();
    outcome.result = Simulator().run(outcome.recipe, outcome.coefficients, outcome.config, is_cancelled);
    outcome.wall_time_ms = elapsed_ms(started);
    artifact_io::stamp_manifest(outcome.result, outcome.recipe, outcome.coefficients, outcome.config);

    if (!request.out_dir.empty()) {
        artifact_io::write_shot_artifacts(request.out_dir, outcome.recipe, outcome.coefficients, outcome.result);
        outcome.artifacts_dir = std::filesystem::absolute(request.out_dir);
    }
    return outcome;
}

// -------------------------------------------------------------------- sweep --
SweepOutcome run_sweep(const SweepRequest& request, const SweepProgressCallback& on_progress) {
    SweepOutcome outcome;
    const SweepSpec spec = artifact_io_sweep::load_sweep_spec_file(request.spec_path);
    const auto started = std::chrono::steady_clock::now();
    outcome.result = ExperimentRunner().run(spec, on_progress);
    outcome.wall_time_ms = elapsed_ms(started);
    if (!request.out_dir.empty()) {
        artifact_io_sweep::write_sweep_artifacts(request.out_dir, outcome.result);
        outcome.artifacts_dir = std::filesystem::absolute(request.out_dir);
    }
    return outcome;
}

// -------------------------------------------------------------- synthesize --
SynthesizeOutcome run_synthesize(const SynthesizeRequest& request, const CancellationCallback& is_cancelled) {
    SynthesizeOutcome outcome;
    outcome.recipe = artifact_io::load_recipe_file(request.recipe_path);
    ModelCoefficients coefficients;
    if (!request.coefficients_path.empty()) {
        coefficients = artifact_io::load_coefficients_file(request.coefficients_path);
    }
    const SimulationConfig config;
    outcome.result = Simulator().run(outcome.recipe, coefficients, config, is_cancelled);
    artifact_io::stamp_manifest(outcome.result, outcome.recipe, coefficients, config);

    const std::string provenance_path =
        request.recipe_path_for_provenance.empty() ? std::string() : request.recipe_path_for_provenance;
    write_text_file(request.out_path, calibration::io::dump_synthetic_shot_json(
                                          outcome.recipe, outcome.result, provenance_path,
                                          request.noise_g, request.seed));
    outcome.out_path = std::filesystem::absolute(request.out_path);
    return outcome;
}

// ------------------------------------------------------------------- bench --
BenchOutcome run_bench(const BenchRequest& request, const CancellationCallback& is_cancelled,
                       const BenchProgressCallback& on_progress) {
    if (request.seconds <= 0.0) throw_single_issue("OUT_OF_RANGE", "seconds must be positive", "seconds");
    if (request.repeats <= 0) throw_single_issue("OUT_OF_RANGE", "repeats must be a positive integer", "repeats");

    Recipe recipe = artifact_io::load_recipe_file(request.recipe_path);
    recipe.maximum_time_s = request.seconds;
    recipe.target_beverage_mass_kg.reset();  // always run the full duration

    ModelCoefficients coefficients;
    if (!request.coefficients_path.empty()) {
        coefficients = artifact_io::load_coefficients_file(request.coefficients_path);
    }

    const SimulationConfig config;
    const Simulator simulator;
    throw_if_cancelled(is_cancelled);

    // One untimed run so the first-touch page faults do not land in the sample.
    const ShotResult warmup = simulator.run(recipe, coefficients, config, is_cancelled);

    BenchOutcome outcome;
    outcome.dt_s = config.dt_s;
    outcome.steps = warmup.diagnostics.step_count;
    outcome.samples_ms.reserve(static_cast<std::size_t>(request.repeats));
    for (int i = 0; i < request.repeats; ++i) {
        throw_if_cancelled(is_cancelled);
        const auto started = std::chrono::steady_clock::now();
        const ShotResult result = simulator.run(recipe, coefficients, config, is_cancelled);
        outcome.samples_ms.push_back(elapsed_ms(started));
        outcome.steps = result.diagnostics.step_count;
        if (on_progress) on_progress(i + 1, request.repeats);
    }

    std::sort(outcome.samples_ms.begin(), outcome.samples_ms.end());
    outcome.best_ms = outcome.samples_ms.front();
    outcome.median_ms = outcome.samples_ms[outcome.samples_ms.size() / 2];
    outcome.p95_ms = outcome.samples_ms[static_cast<std::size_t>(
        0.95 * static_cast<double>(outcome.samples_ms.size() - 1))];
    return outcome;
}

// --------------------------------------------------------------- calibrate --
CalibrateOutcome run_calibrate(const CalibrateRequest& request, const CancellationCallback& is_cancelled) {
    calibration::CalibrationSpec spec;
    if (!request.coefficients_path.empty()) {
        spec.starting_point = artifact_io::load_coefficients_file(request.coefficients_path);
    }
    if (request.max_iterations.has_value()) spec.maximum_iterations = *request.max_iterations;

    for (const std::string& name : request.fit_names) {
        const auto parameter = calibration::tunable_parameter(name);
        if (!parameter.has_value()) {
            throw_single_issue("UNKNOWN_PARAMETER_NAME",
                               "'" + name + "' is not a fittable coefficient; run "
                               "`espressolab_cli fit-params` for the list",
                               name);
        }
        spec.parameters.push_back(*parameter);
    }

    if (request.leave_one_out &&
        (spec.parameters.size() != 2 ||
         std::none_of(spec.parameters.begin(), spec.parameters.end(),
                      [](const auto& p) { return p.name == "kozeny_constant"; }) ||
         std::none_of(spec.parameters.begin(), spec.parameters.end(),
                      [](const auto& p) { return p.name == "extraction_rate_ref_s"; }))) {
        throw_single_issue("INVALID_LEAVE_ONE_OUT_PARAMETERS",
                           "--leave-one-out fits only kozeny_constant and extraction_rate_ref_s", "fit");
    }

    CalibrateOutcome outcome;
    std::vector<calibration::MeasuredShot> shots = calibration::io::load_measured_shot_directory(request.shots_dir);

    if (request.leave_one_out) {
        const ValidationResult validation = calibration::validate_leave_one_out_dataset(shots);
        if (!validation.ok()) throw InvalidInputError(validation);
        for (auto& shot : shots) {
            outcome.fitting_ids.push_back(shot.id);
            spec.fitting_shots.push_back(std::move(shot));
        }
    } else {
        std::vector<std::string> unmatched_holdout = request.holdout_ids;
        for (auto& shot : shots) {
            const auto matches = [&](const std::string& name) { return name == shot.id || name == shot.source_stem; };
            const bool held_out = std::any_of(request.holdout_ids.begin(), request.holdout_ids.end(), matches);
            std::erase_if(unmatched_holdout, matches);
            if (held_out) {
                outcome.validation_ids.push_back(shot.id);
                spec.validation_shots.push_back(std::move(shot));
            } else {
                outcome.fitting_ids.push_back(shot.id);
                spec.fitting_shots.push_back(std::move(shot));
            }
        }
        // A holdout that silently matches nothing would report a fit as
        // validated when it never was, which is worse than refusing to run.
        if (!unmatched_holdout.empty()) {
            throw_single_issue("UNKNOWN_HOLDOUT_SHOT",
                               "no measured shot with id or filename '" + unmatched_holdout.front() +
                                   "' in " + request.shots_dir,
                               "holdout");
        }
        if (spec.fitting_shots.empty()) {
            throw_single_issue("NO_FITTING_SHOTS",
                               "no measured shots to fit in " + request.shots_dir +
                                   " (every shot found was held out)",
                               "shots");
        }
    }

    outcome.spec = spec;
    const auto started = std::chrono::steady_clock::now();
    if (request.leave_one_out) {
        outcome.leave_one_out_report = calibration::leave_one_out(spec, {}, is_cancelled);
        if (outcome.leave_one_out_report->passed) outcome.report = calibration::fit(spec, is_cancelled);
    } else {
        outcome.report = calibration::fit(spec, is_cancelled);
    }
    outcome.wall_time_ms = elapsed_ms(started);

    if (!request.report_path.empty()) {
        write_text_file(request.report_path,
                        outcome.leave_one_out_report.has_value()
                            ? calibration::io::dump_leave_one_out_report_json(
                                  outcome.report.has_value() ? &*outcome.report : nullptr,
                                  *outcome.leave_one_out_report)
                            : calibration::io::dump_report_json(*outcome.report));
        outcome.report_written = std::filesystem::absolute(request.report_path);
    }

    const bool leave_one_out_failed =
        outcome.leave_one_out_report.has_value() && !outcome.leave_one_out_report->passed;
    if (!request.out_path.empty()) {
        if (outcome.report.has_value()) {
            const std::string id = request.id.empty() ? std::filesystem::path(request.out_path).stem().string()
                                                       : request.id;
            write_text_file(request.out_path,
                            calibration::io::dump_fitted_coefficients_json(
                                *outcome.report, id, request.coefficient_version, outcome.fitting_ids,
                                outcome.validation_ids,
                                outcome.leave_one_out_report.has_value() ? &*outcome.leave_one_out_report
                                                                        : nullptr));
            outcome.coefficients_written = std::filesystem::absolute(request.out_path);
        } else if (leave_one_out_failed) {
            outcome.coefficients_withheld_by_validation = true;
        }
    }
    return outcome;
}

// -------------------------------------------------------------------- cfd ---
CfdOutcome run_cfd(const CfdRequest& request, const CancellationCallback& is_cancelled) {
    CfdOutcome outcome;
    outcome.recipe = artifact_io::load_recipe_file(request.recipe_path);
    ModelCoefficients coefficients;
    if (!request.coefficients_path.empty()) {
        coefficients = artifact_io::load_coefficients_file(request.coefficients_path);
    }
    if (request.radial_cells.has_value()) outcome.config.mesh.radial_cells = *request.radial_cells;
    if (request.axial_cells.has_value()) outcome.config.mesh.axial_cells = *request.axial_cells;
    if (request.dt_s.has_value()) outcome.config.dt_s = *request.dt_s;

    const auto started = std::chrono::steady_clock::now();
    outcome.result = CfdSolver().run(outcome.recipe, coefficients, outcome.config, is_cancelled);
    outcome.wall_time_ms = elapsed_ms(started);
    return outcome;
}

// ------------------------------------------------------------------ cfd3d ---
Cfd3dOutcome run_cfd3d(const Cfd3dRequest& request, const CancellationCallback& is_cancelled) {
    Cfd3dOutcome outcome;
    outcome.cfd3d_case =
        request.case_path.empty() ? cfd3d_artifact_io::Cfd3dCase{} : cfd3d_artifact_io::load_case_file(request.case_path);
    if (!request.recipe_path.empty()) outcome.cfd3d_case.recipe = artifact_io::load_recipe_file(request.recipe_path);
    if (!request.coefficients_path.empty()) {
        outcome.cfd3d_case.coefficients = artifact_io::load_coefficients_file(request.coefficients_path);
    }

    Cfd3dConfig& config = outcome.cfd3d_case.config;
    if (request.nx.has_value()) config.mesh.nx = *request.nx;
    if (request.ny.has_value()) config.mesh.ny = *request.ny;
    if (request.nz.has_value()) config.mesh.nz = *request.nz;
    if (request.dt_s.has_value()) config.dt_s = *request.dt_s;
    if (request.sample_interval_s.has_value()) config.sample_interval_s = *request.sample_interval_s;
    if (request.snapshot_interval_s.has_value()) config.snapshot_interval_s = *request.snapshot_interval_s;
    if (!request.material_path.empty()) config.material = load_cfd3d_material(request.material_path, config.mesh);

    if (!request.out_dir.empty()) {
        config.snapshot_sink = [&outcome](const Cfd3dSnapshot& snapshot) { outcome.snapshots.push_back(snapshot); };
    }

    const auto started = std::chrono::steady_clock::now();
    outcome.result = Cfd3dSolver().run(outcome.cfd3d_case.recipe, outcome.cfd3d_case.coefficients, config, is_cancelled);
    outcome.wall_time_ms = elapsed_ms(started);

    if (!request.out_dir.empty()) {
        cfd3d_artifact_io::write_artifacts(request.out_dir, outcome.cfd3d_case, outcome.result, outcome.snapshots);
        outcome.manifest = cfd3d_artifact_io::make_manifest(outcome.cfd3d_case, outcome.result, outcome.snapshots);
        outcome.artifacts_dir = std::filesystem::absolute(request.out_dir);
    }
    return outcome;
}

}  // namespace espressolab::cli_workflows
