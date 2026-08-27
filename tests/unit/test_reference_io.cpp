#include <catch_amalgamated.hpp>

#include <fstream>

#include <nlohmann/json.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/reference_io.hpp"

using namespace espressolab;

namespace {

struct TemporaryDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / "espressolab-reference-test";

    TemporaryDirectory() {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};

void write_text(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream stream(path);
    REQUIRE(stream.good());
    stream << contents;
}

}  // namespace

TEST_CASE("the real reference catalogue preserves metadata and missing telemetry", "[references]") {
    const reference_io::Catalogue catalogue = reference_io::load_directory(testing::reference_dir());

    REQUIRE(catalogue.references.size() == 4);
    REQUIRE(catalogue.load_errors.empty());
    REQUIRE_FALSE(catalogue.telemetry_available);
    REQUIRE(catalogue.references.front().id == "real_gagne_eg1_01");
    REQUIRE(catalogue.references.back().id == "real_gagne_niche_06");

    const nlohmann::json& document = catalogue.references.front().document;
    REQUIRE(document.at("source").at("author") == "Jonathan Gagne");
    REQUIRE(document.at("observed").at("tds_filtered_pct") == 5.69);
    REQUIRE(document.at("observed").at("final_shot_time_s").is_null());
    REQUIRE(document.at("timeseries").is_array());
    REQUIRE(document.at("timeseries").empty());
    REQUIRE_FALSE(document.contains("recipe"));

    const nlohmann::json response = nlohmann::json::parse(reference_io::dump_json(catalogue));
    REQUIRE(response.at("references").size() == 4);
    REQUIRE_FALSE(response.at("telemetry_available").get<bool>());
}

TEST_CASE("reference catalogue reports individual file errors", "[references]") {
    TemporaryDirectory directory;
    write_text(directory.path / "manifest.json", R"({
      "schema_version": "1.0",
      "references": [
        {"id": "good", "file": "good.json"},
        {"id": "bad", "file": "bad.json"}
      ]
    })");
    write_text(directory.path / "good.json", R"({
      "schema_version":"1.0","id":"good",
      "source":{"author":"a","experiment":"e","article_url":"u","experiment_log_url":"l","de1_shot_file":"s","data_quality":{}},
      "setup":{"machine":"m","shower_head":"s","basket":"b","profile":"p","target_brew_ratio":2.0,"bloom_time_s":1.0,
        "coffee":{"name":"c","origin":"o","process":"p","varieties":["v"],"elevation_masl":"1000"}},
      "grinder":{"model":"g","burrs":null,"setting":1.0,"rpm":null},
      "observed":{"dose_g":18.0,"final_beverage_mass_g":36.0,"final_shot_time_s":null,"drip_g":1.0,
        "peak_pressure_bar":9.0,"tds_raw_pct":8.0,"tds_filtered_pct":7.0,"tds_uncertainty_pct_points":0.1,
        "extraction_yield_raw_pct":20.0,"extraction_yield_filtered_pct":19.0},
      "timeseries_fields":[],"timeseries":[]
    })");
    write_text(directory.path / "bad.json", "not json");

    const reference_io::Catalogue catalogue = reference_io::load_directory(directory.path);

    REQUIRE(catalogue.references.size() == 1);
    REQUIRE(catalogue.references.front().id == "good");
    REQUIRE(catalogue.load_errors.size() == 1);
    REQUIRE(catalogue.load_errors.front().file == "bad.json");
    REQUIRE(catalogue.load_errors.front().code == "REFERENCE_FILE_INVALID");
}

TEST_CASE("incomplete reference records are reported as file errors", "[references]") {
    TemporaryDirectory directory;
    write_text(directory.path / "manifest.json", R"({
      "schema_version":"1.0",
      "references":[{"id":"incomplete","file":"incomplete.json"}]
    })");
    write_text(directory.path / "incomplete.json", R"({"id":"incomplete"})");

    const reference_io::Catalogue catalogue = reference_io::load_directory(directory.path);

    REQUIRE(catalogue.references.empty());
    REQUIRE(catalogue.load_errors.size() == 1);
    REQUIRE(catalogue.load_errors.front().code == "REFERENCE_FILE_INVALID");
}
