#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace espressolab::reference_io {

struct LoadError : std::runtime_error {
    LoadError(std::string code_, std::string path_, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(code_)), path(std::move(path_)) {}

    std::string code;
    std::string path;
};

struct LoadIssue {
    std::string file;
    std::string code;
    std::string message;
};

struct ReferenceRecord {
    std::string id;
    std::string file;
    nlohmann::json document = nlohmann::json::object();
};

struct Catalogue {
    std::string schema_version = "1.0";
    bool telemetry_available = false;
    std::string limitation =
        "Shot-level metadata is reported; DE1 time series and final shot times are unavailable.";
    std::vector<ReferenceRecord> references;
    std::vector<LoadIssue> load_errors;
};

Catalogue load_directory(const std::filesystem::path& directory);
std::string dump_json(const Catalogue& catalogue, int indent = 2);

}  // namespace espressolab::reference_io
