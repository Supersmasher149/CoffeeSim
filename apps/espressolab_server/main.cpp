#include <atomic>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "espressolab/artifact_io.hpp"
#include "espressolab/experiment.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/units.hpp"
#include "espressolab/version.hpp"

// tool_server owns REST endpoints, jobs, error translation and file
// boundaries. It implements no equations (section 3.1).
namespace {

using namespace espressolab;
using nlohmann::json;

// Section 12.2.
json error_body(const std::string& code, const std::string& message, const std::string& path,
                json details = json::object()) {
    return {{"error",
             {{"code", code}, {"message", message}, {"path", path}, {"details", details}}}};
}

void send_error(httplib::Response& response, int status, const std::string& code,
                const std::string& message, const std::string& path,
                json details = json::object()) {
    response.status = status;
    response.set_content(error_body(code, message, path, std::move(details)).dump(2),
                         "application/json");
}

// Completed runs live in memory for the life of the process. The MVP has no
// account system or cloud storage (1.4), so this is deliberately not a database.
class RunStore {
public:
    void put_shot(const std::string& id, ShotResult result, Recipe recipe,
                  ModelCoefficients coefficients) {
        const std::lock_guard<std::mutex> lock(mutex_);
        shots_[id] = Entry{std::move(result), std::move(recipe), std::move(coefficients)};
    }

    bool shot(const std::string& id, ShotResult& out) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = shots_.find(id);
        if (it == shots_.end()) return false;
        out = it->second.result;
        return true;
    }

    void put_sweep(const std::string& id, SweepResult result) {
        const std::lock_guard<std::mutex> lock(mutex_);
        sweeps_[id] = std::move(result);
    }

    bool sweep(const std::string& id, SweepResult& out) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sweeps_.find(id);
        if (it == sweeps_.end()) return false;
        out = it->second;
        return true;
    }

private:
    struct Entry {
        ShotResult result;
        Recipe recipe;
        ModelCoefficients coefficients;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> shots_;
    std::unordered_map<std::string, SweepResult> sweeps_;
};

SimulationConfig parse_solver_config(const json& root) {
    SimulationConfig config;
    if (root.contains("solver") && root.at("solver").is_object()) {
        const json& solver = root.at("solver");
        config.dt_s = solver.value("dt_s", config.dt_s);
        config.sample_interval_s = solver.value("sample_interval_s", config.sample_interval_s);
    }
    return config;
}

std::string asset_root(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--assets") return argv[i + 1];
    }
    return "assets";
}

int port_from_args(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--port") return std::stoi(argv[i + 1]);
    }
    return 8734;
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path assets = asset_root(argc, argv);
    const int port = port_from_args(argc, argv);

    RunStore store;
    httplib::Server server;

    // The dashboard is served by Vite on another port during development.
    server.set_default_headers({{"Access-Control-Allow-Origin", "*"},
                                {"Access-Control-Allow-Headers", "Content-Type"},
                                {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"}});
    server.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& response) {
        response.status = 204;
    });

    server.Get("/api/v1/health", [&](const httplib::Request&, httplib::Response& response) {
        const json body = {{"status", "ok"},
                           {"solver_version", version::kSolver},
                           {"recipe_schema_version", version::kRecipeSchema},
                           {"result_schema_version", version::kResultSchema},
                           {"asset_root", std::filesystem::absolute(assets).string()},
                           {"sweepable_parameters", supported_parameter_paths()}};
        response.set_content(body.dump(2), "application/json");
    });

    server.Get("/api/v1/recipes", [&](const httplib::Request&, httplib::Response& response) {
        json list = json::array();
        const std::filesystem::path directory = assets / "recipes";
        if (!std::filesystem::exists(directory)) {
            send_error(response, 500, "ASSETS_NOT_FOUND",
                       "recipe directory not found; start the server with --assets <dir>",
                       directory.string());
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() != ".json") continue;
            try {
                const Recipe recipe = artifact_io::load_recipe_file(entry.path());
                list.push_back({{"id", entry.path().stem().string()},
                                {"name", recipe.name},
                                {"recipe", json::parse(artifact_io::dump_recipe_json(recipe, -1))}});
            } catch (const artifact_io::LoadError& e) {
                list.push_back({{"id", entry.path().stem().string()},
                                {"error", {{"code", e.code}, {"message", e.what()}}}});
            }
        }
        // Stable order so the dashboard's recipe list does not shuffle.
        std::sort(list.begin(), list.end(), [](const json& a, const json& b) {
            return a.at("id").get<std::string>() < b.at("id").get<std::string>();
        });
        response.set_content(json({{"recipes", list}}).dump(2), "application/json");
    });

    server.Post("/api/v1/shots", [&](const httplib::Request& request, httplib::Response& response) {
        json root;
        try {
            root = json::parse(request.body);
        } catch (const json::parse_error& e) {
            send_error(response, 400, "MALFORMED_JSON", e.what(), "body");
            return;
        }

        try {
            if (!root.contains("recipe")) {
                send_error(response, 400, "MISSING_FIELD", "recipe object is required", "recipe");
                return;
            }
            const Recipe recipe = artifact_io::load_recipe_json(root.at("recipe").dump());

            ModelCoefficients coefficients;
            if (root.contains("coefficients")) {
                coefficients = artifact_io::load_coefficients_json(root.at("coefficients").dump());
            } else {
                const std::filesystem::path file = assets / "coefficients" / "default-v1.json";
                if (std::filesystem::exists(file)) {
                    coefficients = artifact_io::load_coefficients_file(file);
                }
            }

            const SimulationConfig config = parse_solver_config(root);
            ShotResult result = Simulator().run(recipe, coefficients, config);
            artifact_io::stamp_manifest(result, recipe, coefficients, config);
            store.put_shot(result.manifest.run_id, result, recipe, coefficients);

            response.status = 201;
            response.set_content(artifact_io::dump_result_json(result), "application/json");
        } catch (const artifact_io::LoadError& e) {
            send_error(response, 400, e.code, e.what(), e.path);
        } catch (const InvalidInputError& e) {
            const auto& issues = e.validation().issues();
            json details = json::array();
            for (const auto& issue : issues) {
                details.push_back({{"code", issue.code}, {"message", issue.message},
                                   {"path", issue.path}});
            }
            send_error(response, 422, issues.empty() ? "INVALID_INPUT" : issues.front().code,
                       issues.empty() ? e.what() : issues.front().message,
                       issues.empty() ? "recipe" : issues.front().path,
                       {{"issues", details}});
        }
    });

    server.Get(R"(/api/v1/shots/([A-Za-z0-9\-]+))",
               [&](const httplib::Request& request, httplib::Response& response) {
                   const std::string id = request.matches[1];
                   ShotResult result;
                   if (!store.shot(id, result)) {
                       send_error(response, 404, "RUN_NOT_FOUND", "no run with id '" + id + "'",
                                  "shots.id");
                       return;
                   }
                   response.set_content(artifact_io::dump_result_json(result), "application/json");
               });

    server.Get(R"(/api/v1/artifacts/([A-Za-z0-9\-]+)\.csv)",
               [&](const httplib::Request& request, httplib::Response& response) {
                   const std::string id = request.matches[1];
                   ShotResult shot;
                   if (store.shot(id, shot)) {
                       response.set_content(artifact_io::dump_samples_csv(shot), "text/csv");
                       return;
                   }
                   SweepResult sweep;
                   if (store.sweep(id, sweep)) {
                       response.set_content(artifact_io_sweep::dump_aggregate_csv(sweep),
                                            "text/csv");
                       return;
                   }
                   send_error(response, 404, "ARTIFACT_NOT_FOUND",
                              "no shot or sweep with id '" + id + "'", "artifacts.id");
               });

    server.Post("/api/v1/sweeps", [&](const httplib::Request& request, httplib::Response& response) {
        json root;
        try {
            root = json::parse(request.body);
        } catch (const json::parse_error& e) {
            send_error(response, 400, "MALFORMED_JSON", e.what(), "body");
            return;
        }

        try {
            SweepSpec spec;
            spec.name = root.value("name", std::string("sweep"));
            if (!root.contains("baseline")) {
                send_error(response, 400, "MISSING_FIELD", "baseline recipe is required",
                           "baseline");
                return;
            }
            spec.baseline = artifact_io::load_recipe_json(root.at("baseline").dump());
            if (root.contains("coefficients")) {
                spec.coefficients = artifact_io::load_coefficients_json(root.at("coefficients").dump());
            } else {
                const std::filesystem::path file = assets / "coefficients" / "default-v1.json";
                if (std::filesystem::exists(file)) {
                    spec.coefficients = artifact_io::load_coefficients_file(file);
                }
            }
            spec.config = parse_solver_config(root);

            std::size_t total = 1;
            for (const auto& axis_json : root.value("axes", json::array())) {
                SweepAxis axis;
                axis.parameter_path = axis_json.value("parameter_path", std::string());
                axis.values = axis_json.value("values", std::vector<double>{});
                total *= std::max<std::size_t>(axis.values.size(), 1);
                spec.axes.push_back(std::move(axis));
            }

            // Sweeps run synchronously in the MVP, so the request is bounded
            // rather than queued as a background job (15.2, scope cut order).
            constexpr std::size_t kMaxSynchronousRuns = 400;
            if (total > kMaxSynchronousRuns) {
                send_error(response, 413, "SWEEP_TOO_LARGE",
                           "this build runs sweeps synchronously; reduce the axes to at most " +
                               std::to_string(kMaxSynchronousRuns) + " runs",
                           "axes", {{"requested_runs", total}});
                return;
            }

            SweepResult result = ExperimentRunner().run(spec);
            store.put_sweep(result.sweep_id, result);

            json body = json::parse(artifact_io_sweep::dump_sweep_json(result, -1));
            json runs = json::array();
            for (const auto& line : result.runs) {
                runs.push_back({{"index", line.index},
                                {"coordinates", line.coordinates},
                                {"run_id", line.run_id},
                                {"termination", to_string(line.summary.termination)},
                                {"shot_time_s", line.summary.elapsed_time_s},
                                {"beverage_mass_g", units::kg_to_grams(line.summary.beverage_mass_kg)},
                                {"tds_percent", line.summary.tds_fraction * 100.0},
                                {"extraction_yield_percent",
                                 line.summary.extraction_yield_fraction * 100.0},
                                {"warning_count", line.warning_count}});
            }
            body["runs"] = std::move(runs);
            body["status"] = "complete";

            response.status = 201;
            response.set_content(body.dump(2), "application/json");
        } catch (const artifact_io::LoadError& e) {
            send_error(response, 400, e.code, e.what(), e.path);
        } catch (const InvalidInputError& e) {
            const auto& issues = e.validation().issues();
            send_error(response, 422, issues.empty() ? "INVALID_INPUT" : issues.front().code,
                       issues.empty() ? e.what() : issues.front().message,
                       issues.empty() ? "axes" : issues.front().path);
        }
    });

    server.Get(R"(/api/v1/sweeps/([A-Za-z0-9\-]+))",
               [&](const httplib::Request& request, httplib::Response& response) {
                   const std::string id = request.matches[1];
                   SweepResult result;
                   if (!store.sweep(id, result)) {
                       send_error(response, 404, "SWEEP_NOT_FOUND", "no sweep with id '" + id + "'",
                                  "sweeps.id");
                       return;
                   }
                   json body = json::parse(artifact_io_sweep::dump_sweep_json(result, -1));
                   body["status"] = "complete";
                   body["runs_jsonl"] = artifact_io_sweep::dump_runs_jsonl(result);
                   response.set_content(body.dump(2), "application/json");
               });

    server.set_exception_handler(
        [](const httplib::Request&, httplib::Response& response, std::exception_ptr ep) {
            std::string message = "unhandled server error";
            try {
                if (ep) std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                message = e.what();
            }
            send_error(response, 500, "INTERNAL_ERROR", message, "");
        });

    std::cout << "espressolab_server " << version::kSolver << '\n'
              << "listening on http://127.0.0.1:" << port << "\n"
              << "assets: " << std::filesystem::absolute(assets).string() << '\n';

    if (!server.listen("127.0.0.1", port)) {
        std::cerr << "error PORT_UNAVAILABLE: could not bind 127.0.0.1:" << port << '\n';
        return 1;
    }
    return 0;
}
