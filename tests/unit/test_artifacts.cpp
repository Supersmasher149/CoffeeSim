#include <catch_amalgamated.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/artifact_io.hpp"

using namespace espressolab;

// Section 14.1: schema errors include a stable code and path.
TEST_CASE("malformed JSON fails with a structured error", "[artifacts]") {
    REQUIRE_THROWS_MATCHES(
        artifact_io::load_recipe_json("{ not json"), artifact_io::LoadError,
        Catch::Matchers::Predicate<artifact_io::LoadError>(
            [](const artifact_io::LoadError& e) { return e.code == "MALFORMED_JSON"; }));
}

TEST_CASE("an unknown recipe schema version is rejected", "[artifacts]") {
    // FR-01: unknown versions fail with a clear error.
    REQUIRE_THROWS_MATCHES(
        artifact_io::load_recipe_json(R"({"schema_version":"9.9","puck":{}})"),
        artifact_io::LoadError,
        Catch::Matchers::Predicate<artifact_io::LoadError>([](const artifact_io::LoadError& e) {
            return e.code == "UNSUPPORTED_SCHEMA_VERSION" && e.path == "recipe.schema_version";
        }));
}

TEST_CASE("a missing field names its own path", "[artifacts]") {
    const std::string text = R"({
      "schema_version": "1.0",
      "puck": { "dose_g": 18.0, "basket_diameter_mm": 58.0, "depth_mm": 9.0,
                "particle_spread_factor": 0.55 },
      "pressure_profile_bar": [[0, 9]],
      "temperature_profile_c": [[0, 93]],
      "stop": { "maximum_time_s": 45 }
    })";
    REQUIRE_THROWS_MATCHES(
        artifact_io::load_recipe_json(text), artifact_io::LoadError,
        Catch::Matchers::Predicate<artifact_io::LoadError>([](const artifact_io::LoadError& e) {
            return e.code == "MISSING_FIELD" && e.path == "recipe.puck.particle_diameter_um";
        }));
}

TEST_CASE("wrongly typed recipe fields fail as loader errors", "[artifacts]") {
    REQUIRE_THROWS_MATCHES(
        artifact_io::load_recipe_json(R"({"schema_version": 1})"), artifact_io::LoadError,
        Catch::Matchers::Predicate<artifact_io::LoadError>([](const artifact_io::LoadError& e) {
            return e.code == "MISSING_FIELD" && e.path == "recipe.schema_version";
        }));
}

TEST_CASE("a malformed profile point is rejected with its index", "[artifacts]") {
    const std::string text = R"({
      "schema_version": "1.0",
      "puck": { "dose_g": 18.0, "basket_diameter_mm": 58.0, "depth_mm": 9.0,
                "particle_diameter_um": 350.0, "particle_spread_factor": 0.55 },
      "pressure_profile_bar": [[0, 9], [10]],
      "temperature_profile_c": [[0, 93]],
      "stop": { "maximum_time_s": 45 }
    })";
    REQUIRE_THROWS_MATCHES(
        artifact_io::load_recipe_json(text), artifact_io::LoadError,
        Catch::Matchers::Predicate<artifact_io::LoadError>([](const artifact_io::LoadError& e) {
            return e.code == "MALFORMED_PROFILE_POINT" && e.path == "recipe.pressure_profile_bar[1]";
        }));
}

TEST_CASE("nonphysical values fail validation rather than simulating", "[artifacts]") {
    Recipe recipe = testing::baseline_recipe();
    recipe.particle_diameter_m = 0.0;

    const ValidationResult result = recipe.validate();
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.issues().front().path == "recipe.puck.particle_diameter_um");
    REQUIRE_THROWS_AS(Simulator().run(recipe, testing::baseline_coefficients()), InvalidInputError);
}

TEST_CASE("parallel-region validation rejects nonphysical partitions", "[artifacts]") {
    Recipe recipe = testing::baseline_recipe();
    recipe.parallel_regions = {{0.7, 1.0}, {0.2, 2.0}};

    const ValidationResult result = recipe.validate();
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.issues().front().path == "recipe.parallel_regions");
}

TEST_CASE("a recipe survives a JSON round trip", "[artifacts]") {
    // FR-07: exported JSON can be reloaded.
    const Recipe original = testing::baseline_recipe();
    const Recipe reloaded = artifact_io::load_recipe_json(artifact_io::dump_recipe_json(original));

    REQUIRE(reloaded.name == original.name);
    REQUIRE(reloaded.dose_kg == Catch::Approx(original.dose_kg));
    REQUIRE(reloaded.particle_diameter_m == Catch::Approx(original.particle_diameter_m));
    REQUIRE(reloaded.pressure_pa.points().size() == original.pressure_pa.points().size());
    REQUIRE(reloaded.pressure_pa.sample(8.0) == Catch::Approx(original.pressure_pa.sample(8.0)));
    REQUIRE(artifact_io::recipe_hash(reloaded) == artifact_io::recipe_hash(original));
}

TEST_CASE("coefficients survive a JSON round trip", "[artifacts]") {
    const ModelCoefficients original = testing::baseline_coefficients();
    const ModelCoefficients reloaded =
        artifact_io::load_coefficients_json(artifact_io::dump_coefficients_json(original));

    REQUIRE(reloaded.id == original.id);
    REQUIRE(reloaded.version == original.version);
    REQUIRE(reloaded.kozeny_constant == Catch::Approx(original.kozeny_constant));
    REQUIRE(reloaded.reference_temperature_k == Catch::Approx(original.reference_temperature_k));
    REQUIRE(artifact_io::coefficient_hash(reloaded) == artifact_io::coefficient_hash(original));
}

// Section 10.2: a run stamped "default v1.0.0" has to mean one thing. The CLI
// leaves ModelCoefficients default-constructed when --coefficients is omitted,
// and the server falls back to these values if the asset file is missing, so
// the compiled-in defaults are a second copy of the shipped set. Nothing else
// in the suite exercises them: every other fixture loads the file.
TEST_CASE("compiled-in coefficient defaults match the shipped default set", "[artifacts]") {
    const ModelCoefficients compiled;
    const ModelCoefficients shipped = testing::baseline_coefficients();

    REQUIRE(compiled.id == shipped.id);
    REQUIRE(compiled.version == shipped.version);

    // Named individually so a drift says which knob moved,
    REQUIRE(compiled.kozeny_constant == Catch::Approx(shipped.kozeny_constant));
    REQUIRE(compiled.extraction_rate_ref_s == Catch::Approx(shipped.extraction_rate_ref_s));
    // and hashed so a drift in any other value cannot slip through unnamed.
    REQUIRE(artifact_io::coefficient_hash(compiled) == artifact_io::coefficient_hash(shipped));
}

TEST_CASE("coefficient JSON requires every typed serialized value", "[artifacts]") {
    const ModelCoefficients original = testing::baseline_coefficients();
    std::string document = artifact_io::dump_coefficients_json(original);
    const std::size_t kozeny = document.find("\"kozeny_constant\":");
    REQUIRE(kozeny != std::string::npos);
    const std::size_t next_value = document.find(',', kozeny);
    REQUIRE(next_value != std::string::npos);
    document.erase(kozeny, next_value - kozeny + 1);

    REQUIRE_THROWS_MATCHES(
        artifact_io::load_coefficients_json(document), artifact_io::LoadError,
        Catch::Matchers::Predicate<artifact_io::LoadError>([](const artifact_io::LoadError& e) {
            return e.code == "MISSING_FIELD" && e.path == "coefficients.values.kozeny_constant";
        }));

    document = artifact_io::dump_coefficients_json(original);
    const std::size_t max_flow = document.find("\"maximum_flow_m3_s\":");
    REQUIRE(max_flow != std::string::npos);
    const std::size_t max_flow_end = document.find(',', max_flow);
    REQUIRE(max_flow_end != std::string::npos);
    document.replace(max_flow, max_flow_end - max_flow, "\"maximum_flow_m3_s\":\"fast\"");
    REQUIRE_THROWS_AS(artifact_io::load_coefficients_json(document), artifact_io::LoadError);
}

TEST_CASE("result hashes include saturation", "[artifacts]") {
    const Recipe recipe = testing::baseline_recipe();
    const ModelCoefficients coefficients = testing::baseline_coefficients();
    const SimulationConfig config;
    std::vector<ShotSample> samples(1);

    const std::string dry_hash = artifact_io::result_hash(recipe, coefficients, config, samples);
    samples.front().saturation = 1.0;
    REQUIRE(artifact_io::result_hash(recipe, coefficients, config, samples) != dry_hash);
}

TEST_CASE("result hashes include parallel-region summaries", "[artifacts]") {
    const Recipe recipe = testing::baseline_recipe();
    const ModelCoefficients coefficients = testing::baseline_coefficients();
    const SimulationConfig config;
    std::vector<ShotSample> samples(1);
    std::vector<RegionSummary> regions(1);

    const std::string uniform_hash = artifact_io::result_hash(recipe, coefficients, config, samples, regions);
    regions.front().flow_fraction = 1.0;
    REQUIRE(artifact_io::result_hash(recipe, coefficients, config, samples, regions) != uniform_hash);
}

TEST_CASE("sha256 matches the published test vectors", "[artifacts]") {
    REQUIRE(artifact_io::sha256_hex("") ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(artifact_io::sha256_hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(artifact_io::sha256_hex(std::string(1000, 'a')).size() == 64);
}

TEST_CASE("the samples CSV has a stable column header", "[artifacts]") {
    const ShotResult result =
        Simulator().run(testing::baseline_recipe(), testing::baseline_coefficients());
    const std::string csv = artifact_io::dump_samples_csv(result);

    REQUIRE(csv.rfind("time_s,pressure_bar,inlet_temperature_c,puck_temperature_c,flow_ml_s,"
                      "beverage_mass_g,tds_percent,extraction_yield_percent,saturation\n",
                      0) == 0);
    REQUIRE(std::count(csv.begin(), csv.end(), '\n') == static_cast<long>(result.samples.size()) + 1);
}
