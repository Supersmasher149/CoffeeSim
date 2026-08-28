#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

#include "espressolab/artifact_io.hpp"
#include "espressolab/experiment.hpp"
#include "espressolab/units.hpp"

namespace espressolab::artifact_io_sweep {
namespace {

using nlohmann::json;

json summary_row(const SweepRun& run) {
    return {{"index", run.index},
            {"run_id", run.run_id},
            {"result_hash", run.result_hash},
            {"coordinates", run.coordinates},
            {"termination", to_string(run.summary.termination)},
            {"shot_time_s", run.summary.elapsed_time_s},
            {"beverage_mass_g", units::kg_to_grams(run.summary.beverage_mass_kg)},
            {"average_flow_ml_s", units::m3_s_to_ml_s(run.summary.average_flow_m3_s)},
            {"peak_flow_ml_s", units::m3_s_to_ml_s(run.summary.peak_flow_m3_s)},
            {"tds_percent", run.summary.tds_fraction * 100.0},
            {"extraction_yield_percent", run.summary.extraction_yield_fraction * 100.0},
            {"brew_ratio", run.summary.brew_ratio},
            {"warning_count", run.warning_count}};
}

void write_file(const std::filesystem::path& file, const std::string& contents) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw artifact_io::LoadError("WRITE_FAILED", file.string(), "could not write " + file.string());
    }
    stream << contents;
}

}  // namespace

std::string dump_sweep_json(const SweepResult& result, int indent) {
    json axes = json::array();
    for (const auto& axis : result.axes) {
        axes.push_back({{"parameter_path", axis.parameter_path}, {"values", axis.values}});
    }
    json root = {{"sweep_id", result.sweep_id},
                 {"name", result.name},
                 {"axes", axes},
                 {"run_count", result.runs.size()},
                 {"cancelled", result.cancelled}};
    return root.dump(indent);
}

// One JSON object per line: streams cleanly and diffs readably (10.4).
std::string dump_runs_jsonl(const SweepResult& result) {
    std::ostringstream out;
    for (const auto& run : result.runs) out << summary_row(run).dump() << '\n';
    return out.str();
}

std::string dump_aggregate_csv(const SweepResult& result) {
    std::ostringstream out;
    out << "index";
    for (const auto& axis : result.axes) out << ',' << axis.parameter_path;
    out << ",termination,shot_time_s,beverage_mass_g,average_flow_ml_s,peak_flow_ml_s,"
           "tds_percent,extraction_yield_percent,brew_ratio,warning_count\n";
    out << std::setprecision(10);
    for (const auto& run : result.runs) {
        out << run.index;
        for (const double coordinate : run.coordinates) out << ',' << coordinate;
        out << ',' << to_string(run.summary.termination) << ',' << run.summary.elapsed_time_s << ','
            << units::kg_to_grams(run.summary.beverage_mass_kg) << ','
            << units::m3_s_to_ml_s(run.summary.average_flow_m3_s) << ','
            << units::m3_s_to_ml_s(run.summary.peak_flow_m3_s) << ','
            << run.summary.tds_fraction * 100.0 << ','
            << run.summary.extraction_yield_fraction * 100.0 << ',' << run.summary.brew_ratio << ','
            << run.warning_count << '\n';
    }
    return out.str();
}

SweepSpec load_sweep_spec_file(const std::filesystem::path& file) {
    std::ifstream stream(file);
    if (!stream) {
        throw artifact_io::LoadError("FILE_NOT_FOUND", file.string(), "could not open " + file.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    json root;
    try {
        root = json::parse(buffer.str());
    } catch (const json::parse_error& e) {
        throw artifact_io::LoadError("MALFORMED_JSON", file.string(), e.what());
    }
    // Audit F4, issue #6: a root of the wrong type (e.g. `[]`) passed parsing
    // but then threw an uncaught nlohmann::json::type_error on the first
    // `.value()` call below instead of a structured error.
    if (!root.is_object()) {
        throw artifact_io::LoadError("MALFORMED_JSON", "sweep", "sweep spec must be a JSON object");
    }

    // Everything below reads fields with unchecked `.value()`/`.get<T>()`
    // calls; a field present with the wrong type (a string "axes", a
    // non-numeric "dt_s", ...) throws nlohmann::json::type_error. Translate
    // any such exception into the project's LoadError contract instead of
    // letting it reach the CLI/server as an uncaught INTERNAL_ERROR.
    // LoadError itself is not a json::exception, so load_recipe_file()'s and
    // load_coefficients_file()'s own errors pass through unchanged.
    try {
        SweepSpec spec;
        spec.name = root.value("name", std::string("sweep"));

        const std::filesystem::path base = file.parent_path();
        const auto resolve = [&base](const std::string& p) {
            const std::filesystem::path candidate(p);
            return candidate.is_absolute() ? candidate : base / candidate;
        };

        if (!root.contains("baseline_recipe")) {
            throw artifact_io::LoadError("MISSING_FIELD", "sweep.baseline_recipe",
                                         "baseline_recipe path is required");
        }
        spec.baseline = artifact_io::load_recipe_file(resolve(root.at("baseline_recipe").get<std::string>()));
        if (root.contains("coefficients")) {
            spec.coefficients =
                artifact_io::load_coefficients_file(resolve(root.at("coefficients").get<std::string>()));
        }
        if (root.contains("solver")) {
            const json& solver = root.at("solver");
            spec.config.dt_s = solver.value("dt_s", spec.config.dt_s);
            spec.config.sample_interval_s =
                solver.value("sample_interval_s", spec.config.sample_interval_s);
        }

        if (!root.contains("axes") || !root.at("axes").is_array()) {
            throw artifact_io::LoadError("MISSING_FIELD", "sweep.axes", "axes array is required");
        }
        for (const auto& axis_json : root.at("axes")) {
            SweepAxis axis;
            axis.parameter_path = axis_json.value("parameter_path", std::string());
            if (axis_json.contains("values")) {
                axis.values = axis_json.at("values").get<std::vector<double>>();
            } else if (axis_json.contains("range")) {
                // {"from": 250, "to": 450, "steps": 9} expands inclusively.
                const json& range = axis_json.at("range");
                const double from = range.value("from", 0.0);
                const double to = range.value("to", 0.0);
                const int steps = range.value("steps", 2);
                for (int i = 0; i < std::max(steps, 1); ++i) {
                    const double f = steps > 1 ? static_cast<double>(i) / (steps - 1) : 0.0;
                    axis.values.push_back(from + f * (to - from));
                }
            }
            spec.axes.push_back(std::move(axis));
        }
        if (root.contains("output_metrics")) {
            spec.output_metrics = root.at("output_metrics").get<std::vector<std::string>>();
        }
        return spec;
    } catch (const json::exception& e) {
        throw artifact_io::LoadError("MALFORMED_JSON", "sweep", e.what());
    }
}

void write_sweep_artifacts(const std::filesystem::path& directory, const SweepResult& result) {
    std::filesystem::create_directories(directory);
    write_file(directory / "sweep.json", dump_sweep_json(result));
    write_file(directory / "runs.jsonl", dump_runs_jsonl(result));
    write_file(directory / "aggregate.csv", dump_aggregate_csv(result));
    write_file(directory / "manifest.json",
               nlohmann::json({{"sweep_id", result.sweep_id},
                               {"name", result.name},
                               {"run_count", result.runs.size()}})
                   .dump(2));
}

}  // namespace espressolab::artifact_io_sweep
