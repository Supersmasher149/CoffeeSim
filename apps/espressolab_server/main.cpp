#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "espressolab/artifact_io.hpp"
#include "espressolab/cfd3d.hpp"
#include "espressolab/cfd3d_artifact_io.hpp"
#include "espressolab/experiment.hpp"
#include "espressolab/reference_io.hpp"
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

// Completed runs are a small, session-local cache rather than a database.
class RunStore {
public:
    void put_shot(const std::string& id, ShotResult result, Recipe recipe,
                  ModelCoefficients coefficients) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = shots_.find(id);
        if (existing != shots_.end()) {
            existing->second = Entry{std::move(result), std::move(recipe), std::move(coefficients)};
            return;
        }
        shots_.emplace(id, Entry{std::move(result), std::move(recipe), std::move(coefficients)});
        order_.push_back(id);
        while (order_.size() > kMaxRetained) {
            shots_.erase(order_.front());
            order_.pop_front();
        }
    }

    bool shot(const std::string& id, ShotResult& out) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = shots_.find(id);
        if (it == shots_.end()) return false;
        out = it->second.result;
        return true;
    }

private:
    static constexpr std::size_t kMaxRetained = 128;
    struct Entry {
        ShotResult result;
        Recipe recipe;
        ModelCoefficients coefficients;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> shots_;
    std::deque<std::string> order_;
};

// Section 15.2 restores background sweep jobs: a sweep no longer blocks the
// request that started it, so the browser can show progress and cancel instead
// of holding a connection open for thousands of runs.
class SweepJob {
public:
    enum class Status { queued, running, complete, cancelled, failed };

    SweepJob(std::string id, SweepSpec spec, int total)
        : id_(std::move(id)), spec_(std::move(spec)), total_(total) {}

    void execute() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            status_ = Status::running;
            started_at_ = std::chrono::steady_clock::now();
        }
        try {
            SweepResult result = ExperimentRunner().run(spec_, [this](int done, int total) {
                const std::lock_guard<std::mutex> lock(mutex_);
                completed_ = done;
                total_ = total;
                return !cancel_requested_;
            });
            const std::lock_guard<std::mutex> lock(mutex_);
            result.sweep_id = id_;
            result_ = std::move(result);
            status_ = result_.cancelled ? Status::cancelled : Status::complete;
        } catch (const InvalidInputError& e) {
            const std::lock_guard<std::mutex> lock(mutex_);
            status_ = Status::failed;
            error_ = e.validation().issues().empty() ? std::string(e.what())
                                                     : e.validation().issues().front().message;
            error_code_ = e.validation().issues().empty()
                              ? "INVALID_INPUT"
                              : e.validation().issues().front().code;
        } catch (const std::exception& e) {
            const std::lock_guard<std::mutex> lock(mutex_);
            status_ = Status::failed;
            error_ = e.what();
            error_code_ = "INTERNAL_ERROR";
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            finished_at_ = std::chrono::steady_clock::now();
            finished_ = true;
        }
        done_.notify_all();
    }

    void cancel() {
        const std::lock_guard<std::mutex> lock(mutex_);
        cancel_requested_ = true;
    }

    // Used on shutdown so a detached worker cannot outlive the store.
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return finished_; });
    }

    struct Snapshot {
        Status status = Status::queued;
        int completed = 0;
        int total = 0;
        double elapsed_s = 0.0;
        std::string error;
        std::string error_code;
        SweepResult result;
    };

    [[nodiscard]] Snapshot snapshot() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        Snapshot out;
        out.status = status_;
        out.completed = completed_;
        out.total = total_;
        out.error = error_;
        out.error_code = error_code_;
        out.result = result_;
        if (status_ != Status::queued) {
            out.elapsed_s = std::chrono::duration<double>(
                                (finished_ ? finished_at_ : std::chrono::steady_clock::now()) -
                                started_at_)
                                .count();
        }
        return out;
    }

    [[nodiscard]] const std::string& id() const { return id_; }

    [[nodiscard]] bool finished() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return finished_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable done_;
    std::string id_;
    SweepSpec spec_;
    Status status_ = Status::queued;
    int completed_ = 0;
    int total_ = 0;
    bool cancel_requested_ = false;
    bool finished_ = false;
    std::string error_;
    std::string error_code_;
    SweepResult result_;
    std::chrono::steady_clock::time_point started_at_{};
    std::chrono::steady_clock::time_point finished_at_{};
};

const char* to_string(SweepJob::Status status) {
    switch (status) {
        case SweepJob::Status::queued: return "queued";
        case SweepJob::Status::running: return "running";
        case SweepJob::Status::complete: return "complete";
        case SweepJob::Status::cancelled: return "cancelled";
        case SweepJob::Status::failed: return "failed";
    }
    return "unknown";
}

class SweepJobStore {
public:
    ~SweepJobStore() { join_all(); }

    std::shared_ptr<SweepJob> start(const std::string& id, SweepSpec spec, int total) {
        reap_finished();
        auto job = std::make_shared<SweepJob>(id, std::move(spec), total);
        const std::lock_guard<std::mutex> lock(mutex_);
        jobs_[id] = job;
        order_.push_back(id);
        // Keep the store from growing without bound over a long session.
        while (order_.size() > kMaxRetained) {
            jobs_.erase(order_.front());
            order_.pop_front();
        }
        // Under the same lock as the registry: two concurrent POSTs would
        // otherwise race on the worker vector.
        workers_.push_back({job, std::thread([job] { job->execute(); })});
        return job;
    }

    [[nodiscard]] std::shared_ptr<SweepJob> find(const std::string& id) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(id);
        return it == jobs_.end() ? nullptr : it->second;
    }

    [[nodiscard]] std::vector<std::shared_ptr<SweepJob>> all() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::shared_ptr<SweepJob>> out;
        for (const auto& id : order_) {
            const auto it = jobs_.find(id);
            if (it != jobs_.end()) out.push_back(it->second);
        }
        return out;
    }

    void join_all() {
        std::vector<Worker> pending;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(workers_);
        }
        for (auto& worker : pending) {
            if (worker.thread.joinable()) worker.thread.join();
        }
    }

private:
    struct Worker {
        std::shared_ptr<SweepJob> job;
        std::thread thread;
    };

    void reap_finished() {
        std::vector<Worker> completed;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            auto worker = workers_.begin();
            while (worker != workers_.end()) {
                if (worker->job->finished()) {
                    completed.push_back(std::move(*worker));
                    worker = workers_.erase(worker);
                } else {
                    ++worker;
                }
            }
        }
        for (auto& worker : completed) {
            if (worker.thread.joinable()) worker.thread.join();
        }
    }

    static constexpr std::size_t kMaxRetained = 32;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<SweepJob>> jobs_;
    std::deque<std::string> order_;
    std::vector<Worker> workers_;
};

class Cfd3dJob {
public:
    enum class Status { queued, running, complete, failed };

    Cfd3dJob(std::string id, cfd3d_artifact_io::Cfd3dCase cfd3d_case)
        : id_(std::move(id)), case_(std::move(cfd3d_case)) {}

    void execute() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            status_ = Status::running;
            started_at_ = std::chrono::steady_clock::now();
        }
        try {
            Cfd3dConfig config = case_.config;
            config.snapshot_sink = [this](const Cfd3dSnapshot& snapshot) {
                const std::lock_guard<std::mutex> lock(mutex_);
                snapshots_.push_back(snapshot);
            };
            Cfd3dResult result = Cfd3dSolver().run(case_.recipe, case_.coefficients, config);
            const std::lock_guard<std::mutex> lock(mutex_);
            result_ = std::move(result);
            status_ = Status::complete;
        } catch (const std::exception& error) {
            const std::lock_guard<std::mutex> lock(mutex_);
            status_ = Status::failed;
            error_ = error.what();
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            finished_at_ = std::chrono::steady_clock::now();
            finished_ = true;
        }
        done_.notify_all();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return finished_; });
    }

    struct Snapshot {
        Status status = Status::queued;
        std::size_t snapshot_count = 0;
        double elapsed_s = 0.0;
        std::string error;
        Cfd3dResult result;
    };

    [[nodiscard]] Snapshot snapshot() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        Snapshot out;
        out.status = status_;
        out.snapshot_count = snapshots_.size();
        out.error = error_;
        out.result = result_;
        if (status_ != Status::queued) {
            out.elapsed_s = std::chrono::duration<double>(
                                (finished_ ? finished_at_ : std::chrono::steady_clock::now()) -
                                started_at_)
                                .count();
        }
        return out;
    }

    [[nodiscard]] const std::string& id() const { return id_; }

    bool snapshot_at(std::size_t index, Cfd3dSnapshot& out) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (status_ != Status::complete || index >= snapshots_.size()) return false;
        out = snapshots_[index];
        return true;
    }

    [[nodiscard]] bool finished() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return finished_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable done_;
    std::string id_;
    cfd3d_artifact_io::Cfd3dCase case_;
    Status status_ = Status::queued;
    bool finished_ = false;
    std::string error_;
    Cfd3dResult result_;
    std::vector<Cfd3dSnapshot> snapshots_;
    std::chrono::steady_clock::time_point started_at_{};
    std::chrono::steady_clock::time_point finished_at_{};
};

const char* to_string(Cfd3dJob::Status status) {
    switch (status) {
        case Cfd3dJob::Status::queued: return "queued";
        case Cfd3dJob::Status::running: return "running";
        case Cfd3dJob::Status::complete: return "complete";
        case Cfd3dJob::Status::failed: return "failed";
    }
    return "unknown";
}

class Cfd3dJobStore {
public:
    ~Cfd3dJobStore() { join_all(); }

    std::shared_ptr<Cfd3dJob> start(const std::string& id,
                                    cfd3d_artifact_io::Cfd3dCase cfd3d_case) {
        reap_finished();
        auto job = std::make_shared<Cfd3dJob>(id, std::move(cfd3d_case));
        const std::lock_guard<std::mutex> lock(mutex_);
        jobs_[id] = job;
        order_.push_back(id);
        while (order_.size() > kMaxRetained) {
            jobs_.erase(order_.front());
            order_.pop_front();
        }
        workers_.push_back({job, std::thread([job] { job->execute(); })});
        return job;
    }

    [[nodiscard]] std::shared_ptr<Cfd3dJob> find(const std::string& id) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = jobs_.find(id);
        return it == jobs_.end() ? nullptr : it->second;
    }

    void join_all() {
        std::vector<Worker> pending;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(workers_);
        }
        for (auto& worker : pending) {
            if (worker.thread.joinable()) worker.thread.join();
        }
    }

private:
    struct Worker {
        std::shared_ptr<Cfd3dJob> job;
        std::thread thread;
    };

    void reap_finished() {
        std::vector<Worker> completed;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            auto worker = workers_.begin();
            while (worker != workers_.end()) {
                if (worker->job->finished()) {
                    completed.push_back(std::move(*worker));
                    worker = workers_.erase(worker);
                } else {
                    ++worker;
                }
            }
        }
        for (auto& worker : completed) {
            if (worker.thread.joinable()) worker.thread.join();
        }
    }

    static constexpr std::size_t kMaxRetained = 4;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Cfd3dJob>> jobs_;
    std::deque<std::string> order_;
    std::vector<Worker> workers_;
};

SimulationConfig parse_solver_config(const json& root) {
    SimulationConfig config;
    if (root.contains("solver")) {
        const json& solver = root.at("solver");
        if (!solver.is_object()) {
            throw artifact_io::LoadError("MALFORMED_JSON", "solver", "solver must be an object");
        }
        const auto optional_number = [&](const char* key, double fallback) {
            if (!solver.contains(key)) return fallback;
            if (!solver.at(key).is_number() || !std::isfinite(solver.at(key).get<double>())) {
                throw artifact_io::LoadError("MALFORMED_JSON", "solver." + std::string(key),
                                             std::string(key) + " must be a number");
            }
            return solver.at(key).get<double>();
        };
        config.dt_s = optional_number("dt_s", config.dt_s);
        config.sample_interval_s = optional_number("sample_interval_s", config.sample_interval_s);
    }
    return config;
}

std::vector<SweepAxis> parse_sweep_axes(const json& root) {
    if (!root.contains("axes")) return {};
    const json& axes = root.at("axes");
    if (!axes.is_array()) {
        throw artifact_io::LoadError("MALFORMED_JSON", "axes", "axes must be an array");
    }
    std::vector<SweepAxis> parsed;
    parsed.reserve(axes.size());
    for (std::size_t index = 0; index < axes.size(); ++index) {
        const json& axis_json = axes[index];
        const std::string path = "axes[" + std::to_string(index) + "]";
        if (!axis_json.is_object()) {
            throw artifact_io::LoadError("MALFORMED_JSON", path, "each axis must be an object");
        }
        if (!axis_json.contains("parameter_path") || !axis_json.at("parameter_path").is_string()) {
            throw artifact_io::LoadError("MALFORMED_JSON", path + ".parameter_path",
                                         "parameter_path must be a string");
        }
        if (!axis_json.contains("values") || !axis_json.at("values").is_array()) {
            throw artifact_io::LoadError("MALFORMED_JSON", path + ".values", "values must be an array");
        }
        SweepAxis axis;
        axis.parameter_path = axis_json.at("parameter_path").get<std::string>();
        for (std::size_t value_index = 0; value_index < axis_json.at("values").size(); ++value_index) {
            const json& value = axis_json.at("values")[value_index];
            if (!value.is_number() || !std::isfinite(value.get<double>())) {
                throw artifact_io::LoadError("MALFORMED_JSON",
                                             path + ".values[" + std::to_string(value_index) + "]",
                                             "axis values must be numbers");
            }
            axis.values.push_back(value.get<double>());
        }
        parsed.push_back(std::move(axis));
    }
    return parsed;
}

std::string route_safe_slug(const std::string& value) {
    std::string slug;
    bool separator_pending = false;
    for (const char character : value) {
        const unsigned char unsigned_character = static_cast<unsigned char>(character);
        if (std::isalnum(unsigned_character)) {
            if (separator_pending && !slug.empty()) slug.push_back('-');
            slug.push_back(static_cast<char>(std::tolower(unsigned_character)));
            separator_pending = false;
        } else if (!slug.empty()) {
            separator_pending = true;
        }
    }
    return slug.empty() ? "sweep" : slug;
}

std::string asset_root(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--assets") return argv[i + 1];
    }
    return "assets";
}

std::string reference_root(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--references") return argv[i + 1];
    }
    return "espresso_real_world_refs";
}

// Audit F9: std::stoi() on a non-numeric --port raised an uncaught
// std::invalid_argument (or std::out_of_range for a huge value) before the
// server entered its own exception handling, aborting the process. Parse and
// range-check the port here so a typo is a controlled startup error instead
// of a crash.
std::optional<int> port_from_args(int argc, char** argv, std::string& error) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--port") {
            const std::string value = argv[i + 1];
            std::size_t consumed = 0;
            long parsed = 0;
            try {
                parsed = std::stol(value, &consumed);
            } catch (const std::exception&) {
                error = "--port must be an integer in [1, 65535], got '" + value + "'";
                return std::nullopt;
            }
            if (consumed != value.size() || parsed < 1 || parsed > 65535) {
                error = "--port must be an integer in [1, 65535], got '" + value + "'";
                return std::nullopt;
            }
            return static_cast<int>(parsed);
        }
    }
    return 8734;
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path assets = asset_root(argc, argv);
    const std::filesystem::path references = reference_root(argc, argv);
    std::string port_error;
    const std::optional<int> parsed_port = port_from_args(argc, argv, port_error);
    if (!parsed_port.has_value()) {
        std::cerr << "error INVALID_ARGUMENT: " << port_error << '\n';
        return 2;
    }
    const int port = *parsed_port;

    RunStore store;
    SweepJobStore sweeps;
    Cfd3dJobStore cfd3d_runs;
    std::atomic<int> next_sweep_serial{1};
    std::atomic<int> next_cfd3d_serial{1};
    httplib::Server server;

    // Serialises a finished sweep for the status and artifact endpoints.
    const auto sweep_body = [](const SweepJob& job) {
        const SweepJob::Snapshot snapshot = job.snapshot();
        json body = {{"sweep_id", job.id()},
                     {"status", to_string(snapshot.status)},
                     {"completed", snapshot.completed},
                     {"total", snapshot.total},
                     {"elapsed_s", snapshot.elapsed_s}};
        if (snapshot.status == SweepJob::Status::failed) {
            body["error"] = {{"code", snapshot.error_code}, {"message", snapshot.error}};
            return body;
        }
        if (snapshot.status != SweepJob::Status::complete &&
            snapshot.status != SweepJob::Status::cancelled) {
            return body;
        }

        const SweepResult& result = snapshot.result;
        body["name"] = result.name;
        body["run_count"] = result.runs.size();
        body["cancelled"] = result.cancelled;

        json axes = json::array();
        for (const auto& axis : result.axes) {
            axes.push_back({{"parameter_path", axis.parameter_path}, {"values", axis.values}});
        }
        body["axes"] = std::move(axes);

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
        return body;
    };

    const auto cfd3d_body = [](const Cfd3dJob& job) {
        const Cfd3dJob::Snapshot snapshot = job.snapshot();
        json body = {{"run_id", job.id()},
                     {"status", to_string(snapshot.status)},
                     {"snapshot_count", snapshot.snapshot_count},
                     {"elapsed_s", snapshot.elapsed_s}};
        if (snapshot.status == Cfd3dJob::Status::failed) {
            body["error"] = {{"code", "CFD3D_FAILED"}, {"message", snapshot.error}};
            return body;
        }
        if (snapshot.status == Cfd3dJob::Status::complete) {
            body["result"] = json::parse(cfd3d_artifact_io::dump_summary_json(snapshot.result, -1));
        }
        return body;
    };

    server.Get("/api/v1/health", [&](const httplib::Request&, httplib::Response& response) {
        const json body = {{"status", "ok"},
                           {"solver_version", version::kSolver},
                           {"recipe_schema_version", version::kRecipeSchema},
                           {"result_schema_version", version::kResultSchema},
                           {"cfd3d_case_schema_version", version::kCfd3dCaseSchema},
                           {"cfd3d_result_schema_version", version::kCfd3dResultSchema},
                           {"cfd3d_field_format", version::kCfd3dFieldFormat},
                           {"asset_root", std::filesystem::absolute(assets).string()},
                           {"reference_root", std::filesystem::absolute(references).string()},
                           {"sweepable_parameters", supported_parameter_paths()}};
        response.set_content(body.dump(2), "application/json");
    });

    server.Get("/api/v1/reference-shots",
               [&](const httplib::Request&, httplib::Response& response) {
                   try {
                       const reference_io::Catalogue catalogue =
                           reference_io::load_directory(references);
                       response.set_content(reference_io::dump_json(catalogue),
                                            "application/json");
                   } catch (const reference_io::LoadError& e) {
                       send_error(response, 500, e.code, e.what(), e.path);
                   }
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

    server.Post("/api/v1/cfd3d/runs",
                [&](const httplib::Request& request, httplib::Response& response) {
                    json root;
                    try {
                        root = json::parse(request.body);
                    } catch (const json::parse_error& e) {
                        send_error(response, 400, "MALFORMED_JSON", e.what(), "body");
                        return;
                    }
                    if (!root.is_object()) {
                        send_error(response, 400, "MALFORMED_JSON",
                                   "request body must be a JSON object", "body");
                        return;
                    }
                    try {
                        if (!root.contains("coefficients")) {
                            const std::filesystem::path file = assets / "coefficients" / "default-v1.json";
                            if (std::filesystem::exists(file)) {
                                root["coefficients"] = json::parse(
                                    artifact_io::dump_coefficients_json(
                                        artifact_io::load_coefficients_file(file), -1));
                            }
                        }
                        const cfd3d_artifact_io::Cfd3dCase cfd3d_case =
                            cfd3d_artifact_io::load_case_json(root.dump());
                        const std::string id =
                            "cfd3d-" + std::to_string(next_cfd3d_serial.fetch_add(1));
                        cfd3d_runs.start(id, cfd3d_case);
                        response.status = 202;
                        response.set_content(
                            json{{"run_id", id}, {"status", "queued"},
                                 {"poll", "/api/v1/cfd3d/runs/" + id}}
                                .dump(2),
                            "application/json");
                    } catch (const artifact_io::LoadError& e) {
                        send_error(response, 400, e.code, e.what(), e.path);
                    } catch (const InvalidInputError& e) {
                        const auto& issues = e.validation().issues();
                        send_error(response, 422,
                                   issues.empty() ? "INVALID_INPUT" : issues.front().code,
                                   issues.empty() ? e.what() : issues.front().message,
                                   issues.empty() ? "cfd3d" : issues.front().path);
                    }
                });

    server.Get(R"(/api/v1/cfd3d/runs/([A-Za-z0-9\-]+))",
               [&](const httplib::Request& request, httplib::Response& response) {
                   const std::string id = request.matches[1];
                   const auto job = cfd3d_runs.find(id);
                   if (!job) {
                       send_error(response, 404, "RUN_NOT_FOUND", "no 3D run with id '" + id + "'",
                                  "cfd3d.id");
                       return;
                   }
                   response.set_content(cfd3d_body(*job).dump(2), "application/json");
               });

    server.Get(R"(/api/v1/cfd3d/runs/([A-Za-z0-9\-]+)/snapshots/([0-9]+))",
               [&](const httplib::Request& request, httplib::Response& response) {
                   const std::string id = request.matches[1];
                   const auto job = cfd3d_runs.find(id);
                   if (!job) {
                       send_error(response, 404, "RUN_NOT_FOUND", "no 3D run with id '" + id + "'",
                                  "cfd3d.id");
                       return;
                   }
                   const auto status = job->snapshot();
                   if (status.status != Cfd3dJob::Status::complete) {
                       send_error(response, 409, "RUN_NOT_FINISHED",
                                  "3D run '" + id + "' is not complete", "cfd3d.id");
                       return;
                   }
                   std::size_t index = 0;
                   try {
                       index = static_cast<std::size_t>(std::stoull(request.matches[2]));
                   } catch (const std::exception&) {
                       send_error(response, 400, "MALFORMED_JSON", "snapshot index is invalid",
                                  "cfd3d.snapshot");
                       return;
                   }
                   Cfd3dSnapshot snapshot;
                   if (!job->snapshot_at(index, snapshot)) {
                       send_error(response, 404, "SNAPSHOT_NOT_FOUND", "snapshot does not exist",
                                  "cfd3d.snapshot");
                       return;
                   }
                   const std::string field_name =
                       request.has_param("field") ? request.get_param_value("field") : "saturation";
                   const Cfd3dField* field = nullptr;
                   if (field_name == "pressure_pa") field = &snapshot.pressure_pa;
                   if (field_name == "saturation") field = &snapshot.saturation;
                   if (field_name == "temperature_k") field = &snapshot.temperature_k;
                   if (field_name == "pore_tds_fraction") field = &snapshot.pore_tds_fraction;
                   if (field_name == "velocity_x_m_s") field = &snapshot.velocity_x_m_s;
                   if (field_name == "velocity_y_m_s") field = &snapshot.velocity_y_m_s;
                   if (field_name == "velocity_z_m_s") field = &snapshot.velocity_z_m_s;
                   if (!field) {
                       send_error(response, 400, "UNKNOWN_FIELD", "unknown 3D field '" + field_name + "'",
                                  "field");
                       return;
                   }
                   response.set_content(
                       json{{"run_id", id},
                            {"snapshot_index", index},
                            {"time_s", snapshot.time_s},
                            {"field", field_name},
                            {"mesh", {{"nx", field->x_cells()},
                                       {"ny", field->y_cells()},
                                       {"nz", field->z_cells()}}},
                            {"ordering", "x-fastest, then y, then z"},
                            {"values", field->values()}}
                           .dump(),
                       "application/json");
               });

    server.Post("/api/v1/shots", [&](const httplib::Request& request, httplib::Response& response) {
        json root;
        try {
            root = json::parse(request.body);
        } catch (const json::parse_error& e) {
            send_error(response, 400, "MALFORMED_JSON", e.what(), "body");
            return;
        }
        if (!root.is_object()) {
            send_error(response, 400, "MALFORMED_JSON", "request body must be a JSON object", "body");
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
                   if (const auto job = sweeps.find(id)) {
                       const SweepJob::Snapshot snapshot = job->snapshot();
                       if (snapshot.status == SweepJob::Status::complete ||
                           snapshot.status == SweepJob::Status::cancelled) {
                           response.set_content(
                               artifact_io_sweep::dump_aggregate_csv(snapshot.result), "text/csv");
                           return;
                       }
                       send_error(response, 409, "SWEEP_NOT_FINISHED",
                                  "sweep '" + id + "' is still " + to_string(snapshot.status),
                                  "artifacts.id");
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
        if (!root.is_object()) {
            send_error(response, 400, "MALFORMED_JSON", "request body must be a JSON object", "body");
            return;
        }

        try {
            SweepSpec spec;
            if (root.contains("name") && !root.at("name").is_string()) {
                throw artifact_io::LoadError("MALFORMED_JSON", "name", "name must be a string");
            }
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

            spec.axes = parse_sweep_axes(root);
            std::size_t total = 1;
            for (const auto& axis : spec.axes) {
                total *= std::max<std::size_t>(axis.values.size(), 1);
            }

            // Sweeps now run on a worker thread, so the limit exists only to
            // keep one request from monopolising the machine, not to keep a
            // connection from timing out.
            constexpr std::size_t kMaxRuns = 20000;
            if (total > kMaxRuns) {
                send_error(response, 413, "SWEEP_TOO_LARGE",
                           "a single sweep is limited to " + std::to_string(kMaxRuns) + " runs",
                           "axes", {{"requested_runs", total}});
                return;
            }

            // Validate the axes up front so an obviously broken sweep fails on
            // this request instead of inside a background job.
            std::unordered_set<std::string> parameter_paths;
            for (const auto& axis : spec.axes) {
                if (axis.values.empty()) {
                    send_error(response, 422, "EMPTY_SWEEP_AXIS",
                               "axis '" + axis.parameter_path + "' has no values", "axes");
                    return;
                }
                if (!parameter_paths.insert(axis.parameter_path).second) {
                    send_error(response, 422, "DUPLICATE_SWEEP_AXIS",
                               "parameter '" + axis.parameter_path + "' appears in more than one axis",
                               "axes");
                    return;
                }
                (void)apply_parameter(spec.baseline, axis.parameter_path, axis.values.front());
            }
            if (spec.axes.empty()) {
                send_error(response, 422, "EMPTY_SWEEP", "a sweep requires at least one axis",
                           "axes");
                return;
            }

            const std::string id = "sweep-" + route_safe_slug(spec.name) + "-" +
                                   std::to_string(next_sweep_serial++);
            sweeps.start(id, std::move(spec), static_cast<int>(total));

            response.status = 202;  // accepted, running in the background
            response.set_content(json({{"sweep_id", id},
                                       {"status", "running"},
                                       {"completed", 0},
                                       {"total", total},
                                       {"poll", "/api/v1/sweeps/" + id}})
                                     .dump(2),
                                 "application/json");
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
                   const auto job = sweeps.find(id);
                   if (!job) {
                       send_error(response, 404, "SWEEP_NOT_FOUND", "no sweep with id '" + id + "'",
                                  "sweeps.id");
                       return;
                   }
                   response.set_content(sweep_body(*job).dump(2), "application/json");
               });

    // Cancellation keeps whatever runs already finished (15.2).
    server.Post(R"(/api/v1/sweeps/([A-Za-z0-9\-]+)/cancel)",
                [&](const httplib::Request& request, httplib::Response& response) {
                    const std::string id = request.matches[1];
                    const auto job = sweeps.find(id);
                    if (!job) {
                        send_error(response, 404, "SWEEP_NOT_FOUND",
                                   "no sweep with id '" + id + "'", "sweeps.id");
                        return;
                    }
                    job->cancel();
                    response.set_content(
                        json({{"sweep_id", id}, {"cancel_requested", true}}).dump(2),
                        "application/json");
                });

    server.Get("/api/v1/sweeps", [&](const httplib::Request&, httplib::Response& response) {
        json list = json::array();
        for (const auto& job : sweeps.all()) {
            const SweepJob::Snapshot snapshot = job->snapshot();
            list.push_back({{"sweep_id", job->id()},
                            {"status", to_string(snapshot.status)},
                            {"completed", snapshot.completed},
                            {"total", snapshot.total},
                            {"elapsed_s", snapshot.elapsed_s}});
        }
        response.set_content(json({{"sweeps", list}}).dump(2), "application/json");
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
    sweeps.join_all();
    return 0;
}
