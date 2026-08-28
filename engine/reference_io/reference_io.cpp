#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "espressolab/reference_io.hpp"

namespace espressolab::reference_io {
namespace {

using nlohmann::json;

std::string read_file(const std::filesystem::path& file) {
    std::ifstream stream(file);
    if (!stream) {
        throw LoadError("REFERENCE_FILE_NOT_FOUND", file.string(),
                        "could not open " + file.string());
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

json parse_file(const std::filesystem::path& file, const std::string& code) {
    try {
        return json::parse(read_file(file));
    } catch (const json::parse_error& e) {
        throw LoadError(code, file.string(), e.what());
    }
}

bool safe_relative_file(const std::filesystem::path& file) {
    if (file.empty() || file.is_absolute()) return false;
    for (const auto& component : file.lexically_normal()) {
        if (component == "..") return false;
    }
    return true;
}

void add_issue(Catalogue& catalogue, const std::string& file, const std::string& code,
               const std::string& message) {
    catalogue.load_errors.push_back({file, code, message});
}

bool require_member(const json& object, const char* key, const std::string& path,
                    bool valid, const char* expected, std::string& error) {
    if (object.contains(key) && valid) return true;
    error = path + " must contain " + expected;
    return false;
}

bool validate_document(const json& document, std::string& error) {
    if (!require_member(document, "schema_version", "schema_version",
                        document.contains("schema_version") &&
                            document.at("schema_version").is_string(),
                        "a string schema_version", error) ||
        !require_member(document, "id", "id",
                        document.contains("id") && document.at("id").is_string(),
                        "a string id", error)) {
        return false;
    }

    const auto require_object = [&](const json& object, const char* key, const std::string& path,
                                    const json*& value) {
        if (!require_member(object, key, path, object.contains(key) && object.at(key).is_object(),
                            "an object", error)) {
            return false;
        }
        value = &object.at(key);
        return true;
    };
    const auto require_string = [&](const json& object, const char* key, const std::string& path) {
        return require_member(object, key, path,
                              object.contains(key) && object.at(key).is_string(), "a string", error);
    };
    const auto require_number = [&](const json& object, const char* key, const std::string& path) {
        return require_member(object, key, path,
                              object.contains(key) && object.at(key).is_number(), "a number", error);
    };

    const json* source = nullptr;
    const json* setup = nullptr;
    const json* coffee = nullptr;
    const json* grinder = nullptr;
    const json* observed = nullptr;
    const json* data_quality = nullptr;
    if (!require_object(document, "source", "source", source) ||
        !require_object(document, "setup", "setup", setup) ||
        !require_object(*setup, "coffee", "setup.coffee", coffee) ||
        !require_object(document, "grinder", "grinder", grinder) ||
        !require_object(document, "observed", "observed", observed) ||
        !require_object(*source, "data_quality", "source.data_quality", data_quality)) {
        return false;
    }

    for (const char* key : {"author", "experiment", "article_url", "experiment_log_url",
                            "de1_shot_file"}) {
        if (!require_string(*source, key, std::string("source.") + key)) return false;
    }
    for (const auto& item : data_quality->items()) {
        if (!item.value().is_string()) {
            error = "source.data_quality values must be strings";
            return false;
        }
    }

    for (const char* key : {"machine", "shower_head", "basket", "profile"}) {
        if (!require_string(*setup, key, std::string("setup.") + key)) return false;
    }
    if (!require_number(*setup, "target_brew_ratio", "setup.target_brew_ratio") ||
        !require_number(*setup, "bloom_time_s", "setup.bloom_time_s")) {
        return false;
    }
    for (const char* key : {"name", "origin", "process", "elevation_masl"}) {
        if (!require_string(*coffee, key, std::string("setup.coffee.") + key)) return false;
    }
    if (!require_member(*coffee, "varieties", "setup.coffee.varieties",
                        coffee->contains("varieties") && coffee->at("varieties").is_array(),
                        "an array", error)) {
        return false;
    }
    for (const json& variety : coffee->at("varieties")) {
        if (!variety.is_string()) {
            error = "setup.coffee.varieties must contain strings";
            return false;
        }
    }

    if (!require_string(*grinder, "model", "grinder.model") ||
        !require_member(*grinder, "burrs", "grinder.burrs",
                        grinder->contains("burrs") &&
                            (grinder->at("burrs").is_string() || grinder->at("burrs").is_null()),
                        "a string or null", error) ||
        !require_number(*grinder, "setting", "grinder.setting") ||
        !require_member(*grinder, "rpm", "grinder.rpm",
                        grinder->contains("rpm") &&
                            (grinder->at("rpm").is_number() || grinder->at("rpm").is_null()),
                        "a number or null", error)) {
        return false;
    }

    for (const char* key : {"dose_g", "final_beverage_mass_g", "drip_g", "peak_pressure_bar",
                            "tds_raw_pct", "tds_filtered_pct", "tds_uncertainty_pct_points",
                            "extraction_yield_raw_pct", "extraction_yield_filtered_pct"}) {
        if (!require_number(*observed, key, std::string("observed.") + key)) return false;
    }
    if (!require_member(*observed, "final_shot_time_s", "observed.final_shot_time_s",
                        observed->contains("final_shot_time_s") &&
                            (observed->at("final_shot_time_s").is_number() ||
                             observed->at("final_shot_time_s").is_null()),
                        "a number or null", error)) {
        return false;
    }

    if (!require_member(document, "timeseries_fields", "timeseries_fields",
                        document.contains("timeseries_fields") &&
                            document.at("timeseries_fields").is_array(),
                        "an array", error) ||
        !require_member(document, "timeseries", "timeseries",
                        document.contains("timeseries") && document.at("timeseries").is_array(),
                        "an array", error)) {
        return false;
    }
    for (const json& field : document.at("timeseries_fields")) {
        if (!field.is_string()) {
            error = "timeseries_fields must contain strings";
            return false;
        }
    }
    return true;
}

}  // namespace

Catalogue load_directory(const std::filesystem::path& directory) {
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        throw LoadError("REFERENCE_CATALOG_NOT_FOUND", directory.string(),
                        "no reference catalogue at " + directory.string());
    }

    const std::filesystem::path manifest_file = directory / "manifest.json";
    if (!std::filesystem::exists(manifest_file)) {
        throw LoadError("REFERENCE_MANIFEST_NOT_FOUND", manifest_file.string(),
                        "reference catalogue has no manifest.json");
    }
    const json manifest = parse_file(manifest_file, "REFERENCE_MANIFEST_INVALID");
    if (!manifest.is_object() || !manifest.contains("references") ||
        !manifest.at("references").is_array()) {
        throw LoadError("REFERENCE_MANIFEST_INVALID", manifest_file.string(),
                        "manifest references must be an array");
    }

    // Audit F4, issue #6: root.value() with a wrong-typed existing key still
    // throws nlohmann::json::type_error (it only tolerates a missing key),
    // which crashed the whole catalogue load with an uncaught exception
    // instead of the manifest-level LoadError the checks just above use for
    // every other structural problem.
    if (manifest.contains("schema_version") && !manifest.at("schema_version").is_string()) {
        throw LoadError("REFERENCE_MANIFEST_INVALID", manifest_file.string(),
                        "manifest schema_version must be a string");
    }
    Catalogue catalogue;
    catalogue.schema_version = manifest.value("schema_version", std::string("1.0"));
    for (std::size_t index = 0; index < manifest.at("references").size(); ++index) {
        const json& entry = manifest.at("references")[index];
        const std::string entry_name = "manifest.references[" + std::to_string(index) + "]";
        if (!entry.is_object() || !entry.contains("id") || !entry.at("id").is_string() ||
            !entry.contains("file") || !entry.at("file").is_string()) {
            add_issue(catalogue, entry_name, "REFERENCE_MANIFEST_ENTRY_INVALID",
                      "each manifest reference needs string id and file fields");
            continue;
        }

        const std::string id = entry.at("id").get<std::string>();
        const std::string relative_name = entry.at("file").get<std::string>();
        const std::filesystem::path relative_file(relative_name);
        const std::filesystem::path file = directory / relative_file;
        if (!safe_relative_file(relative_file)) {
            add_issue(catalogue, relative_name, "REFERENCE_FILE_PATH_INVALID",
                      "reference files must be relative paths inside the catalogue");
            continue;
        }

        try {
            const json document = parse_file(file, "REFERENCE_FILE_INVALID");
            if (!document.is_object()) {
                add_issue(catalogue, relative_name, "REFERENCE_FILE_INVALID",
                          "reference document must be a JSON object");
                continue;
            }
            std::string validation_error;
            if (!validate_document(document, validation_error)) {
                add_issue(catalogue, relative_name, "REFERENCE_FILE_INVALID", validation_error);
                continue;
            }
            const std::string document_id = document.at("id").get<std::string>();
            if (document_id != id) {
                add_issue(catalogue, relative_name, "REFERENCE_ID_MISMATCH",
                          "manifest id does not match the reference document id");
                continue;
            }
            catalogue.references.push_back({document_id, relative_name, document});
        } catch (const LoadError& error) {
            add_issue(catalogue, relative_name, error.code, error.what());
        }
    }
    return catalogue;
}

std::string dump_json(const Catalogue& catalogue, int indent) {
    json references = json::array();
    for (const ReferenceRecord& record : catalogue.references) {
        json document = record.document;
        document["id"] = record.id;
        document["file"] = record.file;
        document["telemetry_available"] = false;
        references.push_back(std::move(document));
    }

    json errors = json::array();
    for (const LoadIssue& issue : catalogue.load_errors) {
        errors.push_back({{"file", issue.file}, {"code", issue.code}, {"message", issue.message}});
    }

    return json{{"schema_version", catalogue.schema_version},
                {"telemetry_available", catalogue.telemetry_available},
                {"limitation", catalogue.limitation},
                {"references", references},
                {"load_errors", errors}}
        .dump(indent);
}

}  // namespace espressolab::reference_io
