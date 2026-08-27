#include <catch_amalgamated.hpp>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/artifact_io.hpp"
#include "espressolab/simulator.hpp"

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

// Audit F5, issue #3: ModelCoefficients::validate() omitted outlet_pressure_pa
// (the field the finding was filed for, despite the schema requiring it to
// be nonnegative) and several other fields entirely. Cover every field this
// fix added checks for, with both a nonphysical finite value and a
// non-finite one where relevant.
TEST_CASE("every previously-omitted coefficient field is validated", "[artifacts]") {
    struct Case {
        std::string name;
        std::string path;
        std::function<void(ModelCoefficients&)> mutate;
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<Case> cases = {
        {"negative outlet pressure", "coefficients.outlet_pressure_pa",
         [](ModelCoefficients& c) { c.outlet_pressure_pa = -100000.0; }},
        {"NaN outlet pressure", "coefficients.outlet_pressure_pa",
         [nan](ModelCoefficients& c) { c.outlet_pressure_pa = nan; }},
        {"negative porosity_compression_factor", "coefficients.porosity_compression_factor",
         [](ModelCoefficients& c) { c.porosity_compression_factor = -0.1; }},
        {"NaN ambient_heat_loss_w_k", "coefficients.ambient_heat_loss_w_k",
         [nan](ModelCoefficients& c) { c.ambient_heat_loss_w_k = nan; }},
        {"negative ambient_temperature_k", "coefficients.ambient_temperature_k",
         [](ModelCoefficients& c) { c.ambient_temperature_k = -1.0; }},
        {"non-finite initial_puck_temperature_k", "coefficients.initial_puck_temperature_k",
         [](ModelCoefficients& c) {
             c.initial_puck_temperature_k = std::numeric_limits<double>::infinity();
         }},
        {"NaN activation_energy_j_mol", "coefficients.activation_energy_j_mol",
         [nan](ModelCoefficients& c) { c.activation_energy_j_mol = nan; }},
        {"distribution_factor_floor out of range", "coefficients.distribution_factor_floor",
         [](ModelCoefficients& c) { c.distribution_factor_floor = 0.0; }},
    };

    for (const Case& test_case : cases) {
        SECTION(test_case.name) {
            ModelCoefficients coefficients = testing::baseline_coefficients();
            test_case.mutate(coefficients);

            const ValidationResult result = coefficients.validate();
            REQUIRE_FALSE(result.ok());
            REQUIRE(result.issues().front().path == test_case.path);
            REQUIRE_THROWS_AS(Simulator().run(testing::baseline_recipe(), coefficients), InvalidInputError);
        }
    }
}

TEST_CASE("parallel-region validation rejects nonphysical partitions", "[artifacts][regions]") {
    Recipe recipe = testing::baseline_recipe();

    SECTION("area fractions that do not sum to one") {
        recipe.parallel_regions = {{0.7, 1.0}, {0.2, 2.0}};
        const ValidationResult result = recipe.validate();
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.issues().front().path == "recipe.parallel_regions");
    }

    SECTION("an empty partition") {
        recipe.parallel_regions.clear();
        const ValidationResult result = recipe.validate();
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.issues().front().path == "recipe.parallel_regions");
    }

    SECTION("more than eight regions") {
        recipe.parallel_regions.assign(9, {1.0 / 9.0, 1.0});
        const ValidationResult result = recipe.validate();
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.issues().front().path == "recipe.parallel_regions");
    }

    SECTION("a permeability multiplier outside the validated range") {
        recipe.parallel_regions = {{0.5, 1.0}, {0.5, 25.0}};
        const ValidationResult result = recipe.validate();
        REQUIRE_FALSE(result.ok());
        // The failing region names itself by index, not just the array.
        REQUIRE(result.issues().front().path ==
                "recipe.parallel_regions[1].permeability_multiplier");
    }

    SECTION("an individual area fraction below the floor") {
        recipe.parallel_regions = {{0.995, 1.0}, {0.005, 1.0}};
        const ValidationResult result = recipe.validate();
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.issues().front().path == "recipe.parallel_regions[1].area_fraction");
    }

    SECTION("a legal eight-way split is accepted") {
        recipe.parallel_regions.assign(8, {0.125, 1.0});
        REQUIRE(recipe.validate().ok());
    }
}

TEST_CASE("axial cell counts outside the supported range are rejected", "[artifacts][axial]") {
    Recipe recipe = testing::baseline_recipe();

    SECTION("zero cells") {
        recipe.axial_cells = 0;
        const ValidationResult result = recipe.validate();
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.issues().front().path == "recipe.axial_cells");
    }

    SECTION("a negative count") {
        recipe.axial_cells = -4;
        REQUIRE_FALSE(recipe.validate().ok());
    }

    SECTION("more cells than the solver supports") {
        recipe.axial_cells = 33;
        const ValidationResult result = recipe.validate();
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.issues().front().path == "recipe.axial_cells");
    }

    SECTION("the supported bounds are accepted") {
        recipe.axial_cells = 1;
        REQUIRE(recipe.validate().ok());
        recipe.axial_cells = 32;
        REQUIRE(recipe.validate().ok());
    }
}

TEST_CASE("axial_cells survives a recipe round trip", "[artifacts][axial]") {
    Recipe original = testing::baseline_recipe();
    original.axial_cells = 12;
    const Recipe reloaded = artifact_io::load_recipe_json(artifact_io::dump_recipe_json(original));

    REQUIRE(reloaded.axial_cells == 12);
    REQUIRE(artifact_io::recipe_hash(reloaded) == artifact_io::recipe_hash(original));

    // A recipe written before Level 3 has no field and must load as one cell.
    const Recipe legacy = testing::baseline_recipe();
    REQUIRE(legacy.axial_cells == 1);
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

// The hash test below builds RegionSummary values by hand, so it would still
// pass if the serializer stopped emitting them. This one runs a real two-region
// shot and reads the region contract back out of the JSON.
TEST_CASE("region summaries reach the serialized artifacts", "[artifacts][regions]") {
    const Recipe recipe = testing::channelled_recipe();
    const ShotResult result = Simulator().run(recipe, testing::baseline_coefficients());
    REQUIRE(result.regions.size() == 2);

    for (const std::string& document :
         {artifact_io::dump_summary_json(result), artifact_io::dump_result_json(result)}) {
        const nlohmann::json root = nlohmann::json::parse(document);
        REQUIRE(root.contains("regions"));
        const nlohmann::json& regions = root.at("regions");
        REQUIRE(regions.is_array());
        REQUIRE(regions.size() == recipe.parallel_regions.size());

        double flow_fraction_total = 0.0;
        for (std::size_t i = 0; i < regions.size(); ++i) {
            const nlohmann::json& region = regions[i];
            // The configured partition is echoed back in recipe order.
            REQUIRE(region.at("area_fraction").get<double>() ==
                    Catch::Approx(recipe.parallel_regions[i].area_fraction));
            REQUIRE(region.at("permeability_multiplier").get<double>() ==
                    Catch::Approx(recipe.parallel_regions[i].permeability_multiplier));
            for (const char* field : {"beverage_mass_g", "flow_fraction", "tds_percent",
                                      "extraction_yield_percent"}) {
                REQUIRE(region.contains(field));
                REQUIRE(std::isfinite(region.at(field).get<double>()));
            }
            flow_fraction_total += region.at("flow_fraction").get<double>();
        }
        REQUIRE(flow_fraction_total == Catch::Approx(1.0).margin(1.0e-9));

        // channelled.json gives its narrow region four times the permeability,
        // so that region must carry the larger share of the flow.
        REQUIRE(regions[1].at("flow_fraction").get<double>() >
                regions[0].at("flow_fraction").get<double>());
    }

    // Samples stay aggregate: the CSV export is the dashboard contract and
    // gains no per-region columns at Level 2.
    const std::string csv = artifact_io::dump_samples_csv(result);
    REQUIRE(csv.find("region") == std::string::npos);
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
