#include <catch_amalgamated.hpp>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/calibration.hpp"

using namespace espressolab;
using namespace espressolab::calibration;

namespace {

using nlohmann::json;

std::filesystem::path test_directory() {
    return std::filesystem::temp_directory_path() / "espressolab-calibration-io-test";
}

json measured_document() {
    return {{"schema_version", "1.0"},
            {"id", "shot-a"},
            {"recipe", json::parse(artifact_io::dump_recipe_json(testing::baseline_recipe(), -1))},
            {"synthetic", true},
            {"series",
             {{"time_s", {0.0, 1.0}},
              {"beverage_mass_g", {0.0, 1.0}},
              {"pressure_bar", {nullptr, 9.0}}}},
            {"final",
             {{"beverage_mass_g", 1.0}, {"shot_time_s", nullptr}, {"tds_percent", nullptr}}}};
}

void write_json(const std::filesystem::path& path, const json& document) {
    std::ofstream stream(path);
    REQUIRE(stream.good());
    stream << document.dump(2);
}

}  // namespace

TEST_CASE("measured shot loading preserves null optional measurements", "[calibration][artifacts]") {
    const std::filesystem::path directory = test_directory();
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const std::filesystem::path file = directory / "shot-a.json";
    write_json(file, measured_document());

    const MeasuredShot shot = io::load_measured_shot_file(file, directory);
    REQUIRE(shot.synthetic);
    REQUIRE(shot.series.size() == 2);
    REQUIRE_FALSE(shot.series.front().pressure_bar.has_value());
    REQUIRE(shot.series.back().pressure_bar == Catch::Approx(9.0));
    REQUIRE_FALSE(shot.final_shot_time_s.has_value());
    REQUIRE_FALSE(shot.final_tds_percent.has_value());
    std::filesystem::remove_all(directory);
}

TEST_CASE("measured shot loading translates wrong member types", "[calibration][artifacts]") {
    const std::filesystem::path directory = test_directory();
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const std::filesystem::path file = directory / "bad.json";

    SECTION("series") {
        json document = measured_document();
        document["series"] = "not an object";
        write_json(file, document);
        REQUIRE_THROWS_AS(io::load_measured_shot_file(file, directory), artifact_io::LoadError);
    }
    SECTION("final value") {
        json document = measured_document();
        document["final"]["tds_percent"] = "unknown";
        write_json(file, document);
        REQUIRE_THROWS_AS(io::load_measured_shot_file(file, directory), artifact_io::LoadError);
    }
    SECTION("pressure length") {
        json document = measured_document();
        document["series"]["pressure_bar"] = json::array({9.0});
        write_json(file, document);
        REQUIRE_THROWS_AS(io::load_measured_shot_file(file, directory), artifact_io::LoadError);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE("measured shot directories load in filename order", "[calibration][artifacts]") {
    const std::filesystem::path directory = test_directory();
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    json first = measured_document();
    first["id"] = "z-id";
    json second = measured_document();
    second["id"] = "a-id";
    write_json(directory / "a-file.json", first);
    write_json(directory / "z-file.json", second);

    const std::vector<MeasuredShot> shots = io::load_measured_shot_directory(directory);
    REQUIRE(shots.size() == 2);
    REQUIRE(shots[0].source_stem == "a-file");
    REQUIRE(shots[1].source_stem == "z-file");
    std::filesystem::remove_all(directory);
}

TEST_CASE("calibration loss serialization includes regularization", "[calibration][artifacts]") {
    CalibrationReport report;
    ShotLoss shot_loss;
    shot_loss.shot_id = "shot-a";
    shot_loss.loss.regularization = 3.5;
    report.fitting_losses.push_back(shot_loss);

    const json document = json::parse(io::dump_report_json(report));
    REQUIRE(document.at("fitting_shots").at(0).at("loss").at("regularization") == 3.5);
}
