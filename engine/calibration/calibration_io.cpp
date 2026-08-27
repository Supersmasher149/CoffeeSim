#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>

#include <nlohmann/json.hpp>

#include "espressolab/artifact_io.hpp"
#include "espressolab/calibration.hpp"
#include "espressolab/units.hpp"
#include "espressolab/version.hpp"

namespace espressolab::calibration::io {
namespace {

using nlohmann::json;

std::string read_file(const std::filesystem::path& file) {
    std::ifstream stream(file);
    if (!stream) {
        throw artifact_io::LoadError("FILE_NOT_FOUND", file.string(),
                                     "could not open " + file.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::optional<double> optional_double(const json& node, const char* key) {
    if (!node.contains(key) || node.at(key).is_null() || !node.at(key).is_number()) {
        return std::nullopt;
    }
    return node.at(key).get<double>();
}

json breakdown_json(const LossBreakdown& loss) {
    return {{"mass_rmse_g", loss.mass_rmse_g},
            {"time_error_s", loss.time_error_s},
            {"tds_error_percent", loss.tds_error_percent},
            {"pressure_rmse_bar", loss.pressure_rmse_bar},
            {"total", loss.total},
            {"simulated", loss.simulated},
            {"has_time_measurement", loss.has_time_measurement},
            {"has_tds_measurement", loss.has_tds_measurement},
            {"has_pressure_measurement", loss.has_pressure_measurement}};
}

json leave_one_out_json(const LeaveOneOutReport& report) {
    json folds = json::array();
    for (const ShotLoss& fold : report.folds) {
        folds.push_back({{"held_out_shot_id", fold.shot_id}, {"loss", breakdown_json(fold.loss)}});
    }
    return {{"thresholds",
             {{"median_mass_rmse_g", report.thresholds.median_mass_rmse_g},
              {"median_time_error_s", report.thresholds.median_time_error_s},
              {"median_tds_error_percent", report.thresholds.median_tds_error_percent},
              {"worst_fold_multiplier", report.thresholds.worst_fold_multiplier}}},
            {"folds", folds},
            {"median_mass_rmse_g", report.median_mass_rmse_g},
            {"worst_mass_rmse_g", report.worst_mass_rmse_g},
            {"median_time_error_s", report.median_time_error_s},
            {"worst_time_error_s", report.worst_time_error_s},
            {"tds_assessed", report.tds_assessed},
            {"median_tds_error_percent",
             report.median_tds_error_percent.has_value() ? json(*report.median_tds_error_percent)
                                                          : json(nullptr)},
            {"worst_tds_error_percent",
             report.worst_tds_error_percent.has_value() ? json(*report.worst_tds_error_percent)
                                                         : json(nullptr)},
            {"median_pressure_rmse_bar",
             report.median_pressure_rmse_bar.has_value() ? json(*report.median_pressure_rmse_bar)
                                                          : json(nullptr)},
            {"worst_pressure_rmse_bar",
             report.worst_pressure_rmse_bar.has_value() ? json(*report.worst_pressure_rmse_bar)
                                                         : json(nullptr)},
            {"passed", report.passed},
            {"failed_checks", report.failed_checks}};
}

}  // namespace

MeasuredShot load_measured_shot_file(const std::filesystem::path& file,
                                     const std::filesystem::path& recipe_base) {
    json root;
    try {
        root = json::parse(read_file(file));
    } catch (const json::parse_error& e) {
        throw artifact_io::LoadError("MALFORMED_JSON", file.string(), e.what());
    }

    const std::string schema = root.value("schema_version", std::string("1.0"));
    if (schema != "1.0") {
        throw artifact_io::LoadError("UNSUPPORTED_SCHEMA_VERSION",
                                     file.string() + ".schema_version",
                                     "measured shot schema_version '" + schema +
                                         "' is not supported (expected 1.0)");
    }

    MeasuredShot shot;
    shot.id = root.value("id", file.stem().string());
    shot.source_stem = file.stem().string();
    shot.machine = root.value("machine", std::string());
    shot.date = root.value("date", std::string());
    shot.notes = root.value("notes", std::string());
    shot.synthetic = root.value("synthetic", false);

    if (!root.contains("recipe")) {
        throw artifact_io::LoadError("MISSING_FIELD", file.string() + ".recipe",
                                     "a measured shot must name the recipe it was brewed from");
    }
    const json& recipe_node = root.at("recipe");
    if (recipe_node.is_string()) {
        const std::filesystem::path candidate(recipe_node.get<std::string>());
        const std::filesystem::path resolved =
            candidate.is_absolute() ? candidate : recipe_base / candidate;
        shot.recipe = artifact_io::load_recipe_file(resolved);
    } else {
        // An inline recipe keeps a shot self-contained, which matters when the
        // machine setup is a one-off that no asset file describes.
        shot.recipe = artifact_io::load_recipe_json(recipe_node.dump());
    }

    if (root.contains("series") && root.at("series").is_object()) {
        const json& series = root.at("series");
        const auto times = series.value("time_s", std::vector<double>{});
        const auto masses = series.value("beverage_mass_g", std::vector<double>{});
        const auto pressures = series.value("pressure_bar", std::vector<double>{});
        if (times.size() != masses.size()) {
            throw artifact_io::LoadError(
                "SERIES_LENGTH_MISMATCH", file.string() + ".series",
                "time_s and beverage_mass_g must have the same number of points");
        }
        for (std::size_t i = 0; i < times.size(); ++i) {
            MeasuredSample sample;
            sample.time_s = times[i];
            sample.beverage_mass_g = masses[i];
            if (i < pressures.size()) sample.pressure_bar = pressures[i];
            shot.series.push_back(sample);
        }
    }

    if (root.contains("final") && root.at("final").is_object()) {
        const json& final_node = root.at("final");
        shot.final_beverage_mass_g = optional_double(final_node, "beverage_mass_g");
        shot.final_shot_time_s = optional_double(final_node, "shot_time_s");
        shot.final_tds_percent = optional_double(final_node, "tds_percent");
    }
    return shot;
}

std::vector<MeasuredShot> load_measured_shot_directory(const std::filesystem::path& directory) {
    if (!std::filesystem::exists(directory)) {
        throw artifact_io::LoadError("DIRECTORY_NOT_FOUND", directory.string(),
                                     "no measured-shot directory at " + directory.string());
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".json") files.push_back(entry.path());
    }
    // Sorted so a fit over the same directory is reproducible.
    std::sort(files.begin(), files.end());

    std::vector<MeasuredShot> shots;
    shots.reserve(files.size());
    for (const auto& file : files) {
        try {
            shots.push_back(load_measured_shot_file(file, directory));
        } catch (const artifact_io::LoadError& e) {
            // Naming the file and the expectation beats propagating a bare
            // "missing field" from somewhere inside a directory scan.
            throw artifact_io::LoadError(
                e.code, file.string(),
                file.filename().string() + " could not be read as a measured shot (" +
                    std::string(e.what()) +
                    "). This directory must contain only measured-shot files; write fitted "
                    "coefficients and reports somewhere else.");
        }
    }
    return shots;
}

std::string dump_report_json(const CalibrationReport& report, int indent) {
    json parameters = json::array();
    for (std::size_t i = 0; i < report.parameters.size(); ++i) {
        parameters.push_back({{"name", report.parameters[i].name},
                              {"low", report.parameters[i].low},
                              {"high", report.parameters[i].high},
                              {"logarithmic", report.parameters[i].logarithmic},
                              {"start", report.starting_values[i]},
                              {"fitted", report.fitted_values[i]}});
    }

    json fitting = json::array();
    for (const auto& entry : report.fitting_losses) {
        fitting.push_back({{"shot_id", entry.shot_id}, {"loss", breakdown_json(entry.loss)}});
    }
    json validation = json::array();
    for (const auto& entry : report.validation_losses) {
        validation.push_back({{"shot_id", entry.shot_id}, {"loss", breakdown_json(entry.loss)}});
    }

    json root = {{"solver_version", version::kSolver},
                 {"converged", report.converged},
                 {"iterations", report.iterations},
                 {"simulations", report.simulations},
                 {"starting_loss", report.starting_loss},
                 {"final_loss", report.final_loss},
                 {"validation_loss", report.validation_loss},
                 {"parameters", parameters},
                 {"fitting_shots", fitting},
                 {"validation_shots", validation},
                 {"used_synthetic_data", report.used_synthetic_data}};

    if (report.validation_losses.empty()) {
        root["warning"] =
            "No held-out validation shot. A fit scored only on its own fitting shots "
            "has not been shown to generalise (section 11.3).";
    }
    if (report.used_synthetic_data) {
        root["warning_synthetic"] =
            "Fitted against synthetic data generated by the model itself. This validates "
            "the calibration machinery only; it says nothing about real espresso.";
    }
    return root.dump(indent);
}

std::string dump_leave_one_out_report_json(const CalibrationReport* final_fit,
                                           const LeaveOneOutReport& validation, int indent) {
    json root = {{"refit", final_fit == nullptr ? json(nullptr)
                                                   : json::parse(dump_report_json(*final_fit, -1))},
                 {"leave_one_out", leave_one_out_json(validation)}};
    if (final_fit == nullptr) {
        root["refit_note"] = "Validation failed, so the full-dataset refit was not run.";
    }
    return root.dump(indent);
}

std::string dump_fitted_coefficients_json(const CalibrationReport& report, const std::string& id,
                                           const std::string& version,
                                           const std::vector<std::string>& fitting_shot_ids,
                                           const std::vector<std::string>& validation_shot_ids,
                                           const LeaveOneOutReport* leave_one_out,
                                           int indent) {
    json root = json::parse(artifact_io::dump_coefficients_json(report.fitted, -1));
    root["id"] = id;
    root["version"] = version;

    std::vector<std::string> fitted_names;
    for (const auto& parameter : report.parameters) fitted_names.push_back(parameter.name);

    std::vector<std::string> limitations{
        "Only the listed parameters were fitted; every other value is inherited from the "
        "starting coefficient set and remains uncalibrated.",
        "Estimated TDS and extraction yield are engineering outputs, not flavor scores."};
    if (validation_shot_ids.empty()) {
        limitations.emplace_back(
            "No held-out validation shot: this fit has not been shown to generalise beyond "
            "the shots it was trained on.");
    }
    if (report.used_synthetic_data) {
        limitations.emplace_back(
            "SYNTHETIC DATA. These coefficients were fitted against output generated by the "
            "model itself and must not be presented as calibrated against real espresso.");
    }

    // Issue #9, Audit F6: "dataset" here used to hold the array of fitting
    // shot IDs, but schemas/coefficients.schema.json documents provenance
    // .dataset as a single string or null, and load_coefficients_json() now
    // enforces that shape -- reloading a calibration output file (e.g. as
    // the starting point for a further fit) would otherwise throw
    // MALFORMED_JSON. There is no single named dataset here, so this is
    // null; the shot IDs themselves are preserved under fitting_shots
    // instead, alongside the fields below that are calibration-run
    // telemetry rather than part of the base coefficient contract.
    root["provenance"] = {{"source", report.used_synthetic_data
                                         ? "fitted against SYNTHETIC shots (machinery check only)"
                                         : "fitted against measured shots"},
                          {"dataset", nullptr},
                          {"fitting_shots", fitting_shot_ids},
                          {"validation_shots", validation_shot_ids},
                          {"fitted_parameters", fitted_names},
                          {"final_loss", report.final_loss},
                          {"validation_loss", report.validation_loss},
                           {"solver_version", version::kSolver},
                           {"limitations", limitations}};
    if (leave_one_out != nullptr) {
        root["provenance"]["leave_one_out_validation"] = leave_one_out_json(*leave_one_out);
    }
    return root.dump(indent);
}

std::string dump_synthetic_shot_json(const Recipe& recipe, const ShotResult& result,
                                     const std::string& recipe_path, double noise_g,
                                     unsigned int seed, int indent) {
    // A fixed seed keeps synthetic shots reproducible, which is the only reason
    // they are useful as a regression fixture.
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, std::max(noise_g, 0.0));

    std::vector<double> times;
    std::vector<double> masses;
    std::vector<double> pressures;
    // A real scale logs at roughly 5 Hz, not at the solver's sample rate.
    double next_time = 0.0;
    for (const auto& sample : result.samples) {
        if (sample.time_s + 1.0e-9 < next_time) continue;
        next_time += 0.2;
        times.push_back(sample.time_s);
        const double clean = units::kg_to_grams(sample.beverage_mass_kg);
        masses.push_back(noise_g > 0.0 ? std::max(clean + noise(rng), 0.0) : clean);
        pressures.push_back(units::pa_to_bar(sample.pressure_pa));
    }

    json root = {
        {"schema_version", "1.0"},
        {"synthetic", true},
        {"id", "synthetic-" + result.manifest.run_id},
        {"recipe", recipe_path.empty() ? json(json::parse(artifact_io::dump_recipe_json(recipe, -1)))
                                       : json(recipe_path)},
        {"machine", "SYNTHETIC - generated by espressolab_cli synthesize, not a real machine"},
        {"date", result.manifest.timestamp_utc},
        {"series", {{"time_s", times}, {"beverage_mass_g", masses}, {"pressure_bar", pressures}}},
        {"final",
         {{"beverage_mass_g", units::kg_to_grams(result.summary.beverage_mass_kg)},
          {"shot_time_s", result.summary.elapsed_time_s},
          {"tds_percent", result.summary.tds_fraction * 100.0}}},
        {"notes",
         "SYNTHETIC. Generated by running the model itself, optionally with gaussian scale "
         "noise. Use it to exercise the calibration workflow; never to claim the model has "
         "been calibrated against real espresso."},
        {"generator",
         {{"solver_version", version::kSolver},
          {"source_result_hash", result.manifest.result_hash},
          {"noise_g", noise_g},
          {"seed", seed}}}};
    return root.dump(indent);
}

}  // namespace espressolab::calibration::io
