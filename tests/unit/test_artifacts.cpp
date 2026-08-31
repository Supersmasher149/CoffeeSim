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
#include "espressolab/units.hpp"

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

    // assets/coefficients/default-v1.json carries provenance; issue #9
    // (Audit F6) is specifically that this used to be silently dropped.
    REQUIRE(original.provenance.has_value());
    REQUIRE(reloaded.provenance.has_value());
    REQUIRE(reloaded.provenance->source == original.provenance->source);
    REQUIRE(reloaded.provenance->dataset == original.provenance->dataset);
    REQUIRE(reloaded.provenance->date == original.provenance->date);
    REQUIRE(reloaded.provenance->limitations == original.provenance->limitations);
}

// Audit F6, issue #9: a coefficient document's `provenance` was accepted by
// the schema but had no field in ModelCoefficients to land in, so it never
// survived loading, and dump_coefficients_json() never emitted it even when
// the caller had it -- provenance disappeared the moment coefficients were
// normalized into a run's artifacts. Cover every supported field, both a
// present dataset string and an absent/null one, and confirm the hash used
// for reproducibility (and the "compiled defaults == shipped defaults" test
// above) is unaffected by this purely descriptive metadata.
TEST_CASE("every supported coefficient provenance field survives a JSON round trip", "[artifacts]") {
    SECTION("with a dataset string") {
        ModelCoefficients original = testing::baseline_coefficients();
        original.provenance = CoefficientProvenance{
            "measured on a Gaggia Classic Pro", std::string("2026-08-27-shots"), "2026-08-27",
            {"three shots, one machine", "no held-out validation"}};

        const ModelCoefficients reloaded =
            artifact_io::load_coefficients_json(artifact_io::dump_coefficients_json(original));

        REQUIRE(reloaded.provenance.has_value());
        REQUIRE(reloaded.provenance->source == "measured on a Gaggia Classic Pro");
        REQUIRE(reloaded.provenance->dataset == std::optional<std::string>("2026-08-27-shots"));
        REQUIRE(reloaded.provenance->date == "2026-08-27");
        REQUIRE(reloaded.provenance->limitations ==
                std::vector<std::string>{"three shots, one machine", "no held-out validation"});
        REQUIRE(artifact_io::coefficient_hash(reloaded) == artifact_io::coefficient_hash(original));
    }

    SECTION("with no dataset") {
        ModelCoefficients original = testing::baseline_coefficients();
        original.provenance = CoefficientProvenance{"software defaults", std::nullopt, "2026-08-27", {}};

        const ModelCoefficients reloaded =
            artifact_io::load_coefficients_json(artifact_io::dump_coefficients_json(original));

        REQUIRE(reloaded.provenance.has_value());
        REQUIRE_FALSE(reloaded.provenance->dataset.has_value());
    }

    SECTION("provenance does not change the hash") {
        ModelCoefficients with_provenance = testing::baseline_coefficients();
        ModelCoefficients without_provenance = with_provenance;
        without_provenance.provenance.reset();
        REQUIRE(with_provenance.provenance.has_value());

        REQUIRE(artifact_io::coefficient_hash(with_provenance) ==
                artifact_io::coefficient_hash(without_provenance));
    }

    SECTION("a document with no provenance loads with none") {
        std::string document = artifact_io::dump_coefficients_json(testing::baseline_coefficients());
        nlohmann::json parsed = nlohmann::json::parse(document);
        parsed.erase("provenance");

        const ModelCoefficients reloaded = artifact_io::load_coefficients_json(parsed.dump());
        REQUIRE_FALSE(reloaded.provenance.has_value());
    }
}

TEST_CASE("malformed coefficient provenance fields are rejected", "[artifacts]") {
    const std::string base = artifact_io::dump_coefficients_json(testing::baseline_coefficients());

    SECTION("dataset must be a string or null") {
        nlohmann::json parsed = nlohmann::json::parse(base);
        parsed["provenance"]["dataset"] = std::vector<std::string>{"a", "b"};
        REQUIRE_THROWS_MATCHES(
            artifact_io::load_coefficients_json(parsed.dump()), artifact_io::LoadError,
            Catch::Matchers::Predicate<artifact_io::LoadError>([](const artifact_io::LoadError& e) {
                return e.code == "MALFORMED_JSON" && e.path == "coefficients.provenance.dataset";
            }));
    }

    SECTION("limitations must be an array of strings") {
        nlohmann::json parsed = nlohmann::json::parse(base);
        parsed["provenance"]["limitations"] = std::vector<int>{1, 2};
        REQUIRE_THROWS_MATCHES(
            artifact_io::load_coefficients_json(parsed.dump()), artifact_io::LoadError,
            Catch::Matchers::Predicate<artifact_io::LoadError>([](const artifact_io::LoadError& e) {
                return e.code == "MALFORMED_JSON" && e.path == "coefficients.provenance.limitations";
            }));
    }
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

// Audit: the flavour overlay (bean profiles, sensory axes) is additive and must
// leave every pre-existing run untouched. These expectations were captured from
// the build immediately BEFORE the overlay was written -- that ordering is the
// whole point. Regenerating them from the current build would make this test
// compare the code against itself and pass no matter what moved.
//
// What is pinned, and why it is not the result hash:
//
// recipe_hash and coefficient_hash ARE pinned exactly. They are digests of JSON
// documents serialized at fixed precision, so they are identical on every
// platform, and they are what prove the overlay did not disturb serialization.
//
// result_hash is deliberately NOT pinned to a literal. It digests the sample
// series at 17 significant digits, and the solver's last ulp depends on the
// platform's libm: `exp` in temperature_factor() and `pow` in grind_factor()
// and the Kozeny-Carman permeability are not bit-identical across
// implementations. The baseline shot hashes ff604b48... on Linux x86_64 and
// 80156b2e... on macOS arm64 -- both on this commit AND on main before the
// overlay existed, which is how we know the difference is the toolchain and not
// this change. Pinning either literal would assert a platform, not a contract.
// See docs/data-contracts.md, "Reproducibility is per-toolchain".
//
// So the physical outputs are pinned instead, to a tolerance far tighter than
// any real change to the solver could hide inside, and the hash is checked for
// the property that is actually portable: determinism.
TEST_CASE("beanless runs are unchanged by the flavour overlay", "[artifacts][flavor]") {
    struct PinnedRun {
        const char* recipe_file;
        const char* recipe_hash;
        double elapsed_time_s;
        double beverage_mass_g;
        double tds_percent;
        double extraction_yield_percent;
        std::size_t sample_count;
    };
    // baseline, the PSD path, the multi-region path and the axially resolved
    // path: one pin per branch the solver can take.
    const PinnedRun pinned[] = {
        {"baseline.json", "f744a2ed317ba0cfca42b9bd8940aa049a79f530403923a9f31133d0b858e3ab",
         29.03, 36.014920336720756, 9.086491796595798, 18.180515455259126, 582},
        {"psd-bimodal.json", "727b5fec1b06a07a0a78f5e7098e42935ab86403cfe97ef8e46cc529660f0b60",
         25.27, 36.02204806836168, 7.218196393683204, 14.445234303340628, 507},
        {"channelled.json", "f7bcc9c53d02baacbfdcfac156a358191dedd356c47c02680ce6915670b1fe60",
         23.62, 36.00993362168562, 3.1291147137151554, 6.259956285306808, 474},
        {"axial-resolved.json", "2e9673ba2145030279b6a12fdd118b7e49bd65a855db37018c9ecdaca31edd2e",
         31.67, 36.02487273339713, 10.957352697530098, 21.92984646797042, 635},
    };
    const ModelCoefficients coefficients = testing::baseline_coefficients();
    REQUIRE(artifact_io::coefficient_hash(coefficients) ==
            "e279c1fe7e2e4dd74332ee8dc45ada37420da76d4621e9c19a8d06b47eb06fb8");

    for (const PinnedRun& expected : pinned) {
        INFO("recipe: " << expected.recipe_file);
        const Recipe recipe =
            artifact_io::load_recipe_file(testing::asset_dir() / "recipes" / expected.recipe_file);
        REQUIRE_FALSE(recipe.bean.has_value());
        REQUIRE(artifact_io::recipe_hash(recipe) == expected.recipe_hash);

        ShotResult result = Simulator().run(recipe, coefficients);
        artifact_io::stamp_manifest(result, recipe, coefficients, {});

        // Tight enough that any real change to the solver's arithmetic fails it,
        // loose enough to survive a different libm's last ulp.
        REQUIRE(result.summary.termination == TerminationReason::target_mass_reached);
        REQUIRE(result.summary.elapsed_time_s == Catch::Approx(expected.elapsed_time_s).epsilon(1e-12));
        REQUIRE(units::kg_to_grams(result.summary.beverage_mass_kg) ==
                Catch::Approx(expected.beverage_mass_g).epsilon(1e-9));
        REQUIRE(result.summary.tds_fraction * 100.0 ==
                Catch::Approx(expected.tds_percent).epsilon(1e-9));
        REQUIRE(result.summary.extraction_yield_fraction * 100.0 ==
                Catch::Approx(expected.extraction_yield_percent).epsilon(1e-9));
        REQUIRE(result.samples.size() == expected.sample_count);

        // The portable half of the hash contract: identical inputs, identical
        // hash, within one build. scripts/demo.sh asserts the same thing across
        // two processes.
        ShotResult again = Simulator().run(recipe, coefficients);
        artifact_io::stamp_manifest(again, recipe, coefficients, {});
        REQUIRE(again.manifest.result_hash == result.manifest.result_hash);
        REQUIRE(again.manifest.run_id == result.manifest.run_id);
        REQUIRE(result.manifest.result_hash.size() == 64);
        REQUIRE(result.manifest.solver_version == "solver-0.4.0");

        // The artifacts a beanless run emits are unchanged in shape too.
        REQUIRE_FALSE(result.flavor.has_value());
        const nlohmann::json document =
            nlohmann::json::parse(artifact_io::dump_result_json(result));
        REQUIRE_FALSE(document.contains("flavor"));
        const std::string csv = artifact_io::dump_samples_csv(result);
        REQUIRE(csv.substr(0, csv.find('\n')) ==
                "time_s,pressure_bar,inlet_temperature_c,puck_temperature_c,flow_ml_s,"
                "beverage_mass_g,tds_percent,extraction_yield_percent,saturation");
    }
}
