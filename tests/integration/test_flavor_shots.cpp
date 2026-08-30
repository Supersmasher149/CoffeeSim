#include <catch_amalgamated.hpp>
#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/artifact_io.hpp"
#include "espressolab/flavor.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/version.hpp"

using namespace espressolab;

namespace {

ShotResult run(const Recipe& recipe) {
    return Simulator().run(recipe, testing::baseline_coefficients());
}

Recipe with_bean(const char* id) {
    Recipe recipe = testing::baseline_recipe();
    recipe.bean =
        artifact_io::load_bean_file(testing::asset_dir() / "beans" / (std::string(id) + ".json"));
    return recipe;
}

}  // namespace

// The claim the whole design rests on: the overlay divides the solids the solver
// extracted, so attaching a bean cannot move a single physical number.
TEST_CASE("attaching a bean changes no physical quantity", "[flavor][integration]") {
    const ShotResult plain = run(testing::baseline_recipe());
    const ShotResult flavoured = run(testing::hologram_recipe());

    REQUIRE_FALSE(plain.flavor.has_value());
    REQUIRE(flavoured.flavor.has_value());

    // Bit-for-bit, not Approx: a tolerance here would hide exactly the drift
    // this test exists to catch.
    REQUIRE(flavoured.summary.termination == plain.summary.termination);
    REQUIRE(flavoured.summary.elapsed_time_s == plain.summary.elapsed_time_s);
    REQUIRE(flavoured.summary.beverage_mass_kg == plain.summary.beverage_mass_kg);
    REQUIRE(flavoured.summary.tds_fraction == plain.summary.tds_fraction);
    REQUIRE(flavoured.summary.extraction_yield_fraction ==
            plain.summary.extraction_yield_fraction);
    REQUIRE(flavoured.summary.brew_ratio == plain.summary.brew_ratio);
    REQUIRE(flavoured.summary.average_flow_m3_s == plain.summary.average_flow_m3_s);
    REQUIRE(flavoured.summary.peak_flow_m3_s == plain.summary.peak_flow_m3_s);

    REQUIRE(flavoured.diagnostics.water_mass_residual_kg ==
            plain.diagnostics.water_mass_residual_kg);
    REQUIRE(flavoured.diagnostics.solids_mass_residual_kg ==
            plain.diagnostics.solids_mass_residual_kg);
    REQUIRE(flavoured.diagnostics.clamp_count == plain.diagnostics.clamp_count);
    REQUIRE(flavoured.diagnostics.step_count == plain.diagnostics.step_count);

    REQUIRE(flavoured.samples.size() == plain.samples.size());
    for (std::size_t i = 0; i < plain.samples.size(); ++i) {
        INFO("sample " << i);
        REQUIRE(flavoured.samples[i].time_s == plain.samples[i].time_s);
        REQUIRE(flavoured.samples[i].flow_m3_s == plain.samples[i].flow_m3_s);
        REQUIRE(flavoured.samples[i].beverage_mass_kg == plain.samples[i].beverage_mass_kg);
        REQUIRE(flavoured.samples[i].tds_fraction == plain.samples[i].tds_fraction);
        REQUIRE(flavoured.samples[i].extraction_yield_fraction ==
                plain.samples[i].extraction_yield_fraction);
        REQUIRE(flavoured.samples[i].saturation == plain.samples[i].saturation);
        REQUIRE(flavoured.samples[i].puck_temperature_k == plain.samples[i].puck_temperature_k);
    }

    REQUIRE(flavoured.regions.size() == plain.regions.size());
    for (std::size_t i = 0; i < plain.regions.size(); ++i) {
        REQUIRE(flavoured.regions[i].tds_fraction == plain.regions[i].tds_fraction);
        REQUIRE(flavoured.regions[i].extraction_yield_fraction ==
                plain.regions[i].extraction_yield_fraction);
    }

    // samples.csv is a fixed contract; the flavour series is its own artifact.
    REQUIRE(artifact_io::dump_samples_csv(flavoured) == artifact_io::dump_samples_csv(plain));
}

TEST_CASE("the overlay closes its own mass balance", "[flavor][integration][invariants]") {
    const ShotResult result = run(testing::hologram_recipe());
    REQUIRE(result.flavor.has_value());

    // The class pools must account for the solids in the cup and nothing else.
    REQUIRE(std::abs(result.flavor->summary.composition_residual) < 1.0e-12);
    REQUIRE(result.flavor->summary.class_clamp_count == 0);

    REQUIRE(result.flavor->series.size() == result.samples.size());
    for (const FlavorSample& sample : result.flavor->series) {
        double total = 0.0;
        for (double share : sample.composition) {
            REQUIRE(std::isfinite(share));
            REQUIRE(share >= 0.0);
            total += share;
        }
        // Either nothing has reached the cup yet, or the shares partition it.
        REQUIRE((total == Catch::Approx(0.0) || total == Catch::Approx(1.0).margin(1.0e-9)));
        for (double value : sample.intensity) {
            REQUIRE(std::isfinite(value));
            REQUIRE(value >= 0.0);
            REQUIRE(value <= kIntensityMax);
        }
    }
}

// The one behavioural claim the tracer exists to make: fast classes lead, slow
// classes follow, so the cup drifts from bright toward bitter as the shot runs.
TEST_CASE("the cup drifts from fast classes to slow ones", "[flavor][integration]") {
    const ShotResult result = run(testing::hologram_recipe());
    REQUIRE(result.flavor.has_value());

    std::vector<FlavorSample> poured;
    for (const FlavorSample& sample : result.flavor->series) {
        if (sample.composition[static_cast<std::size_t>(SoluteClass::acids)] > 0.0) {
            poured.push_back(sample);
        }
    }
    REQUIRE(poured.size() > 20);

    for (std::size_t i = 1; i < poured.size(); ++i) {
        INFO("sample " << i << " at t=" << poured[i].time_s);
        REQUIRE(poured[i].composition[static_cast<std::size_t>(SoluteClass::acids)] <=
                poured[i - 1].composition[static_cast<std::size_t>(SoluteClass::acids)] + 1.0e-12);
        REQUIRE(poured[i].composition[static_cast<std::size_t>(SoluteClass::polyphenols)] >=
                poured[i - 1].composition[static_cast<std::size_t>(SoluteClass::polyphenols)] -
                    1.0e-12);
    }
    // The drift has to be worth reporting, not a rounding artefact.
    const std::size_t acids = static_cast<std::size_t>(SoluteClass::acids);
    REQUIRE(poured.front().composition[acids] - poured.back().composition[acids] > 0.02);
}

// Relative comparisons only. No absolute intensity is asserted, because no
// absolute intensity means anything yet -- the same stance the shot tests take
// with their sanity bands.
TEST_CASE("the catalogue's beans separate the way their roasts imply",
          "[flavor][integration]") {
    const ShotResult light = run(with_bean("ethiopia-dhiba-bate-natural-light"));
    const ShotResult medium = run(with_bean("counter-culture-hologram"));
    const ShotResult dark = run(with_bean("continental-dark-roast"));

    const auto axis = [](const ShotResult& result, SensoryAxis which) {
        return result.flavor->summary.axes[static_cast<std::size_t>(which)].intensity;
    };

    // Light natural leads on fruit and acidity; dark roast leads on chocolate.
    REQUIRE(axis(light, SensoryAxis::fruit) > axis(medium, SensoryAxis::fruit));
    REQUIRE(axis(medium, SensoryAxis::fruit) > axis(dark, SensoryAxis::fruit));
    REQUIRE(axis(light, SensoryAxis::acidity) > axis(dark, SensoryAxis::acidity));
    REQUIRE(axis(dark, SensoryAxis::chocolate) > axis(medium, SensoryAxis::chocolate));
    REQUIRE(axis(medium, SensoryAxis::chocolate) > axis(light, SensoryAxis::chocolate));
    REQUIRE(axis(dark, SensoryAxis::bitterness) > axis(light, SensoryAxis::bitterness));

    // The physics is identical across all three: only the reported flavour moves.
    REQUIRE(light.summary.tds_fraction == dark.summary.tds_fraction);
    REQUIRE(light.summary.extraction_yield_fraction == dark.summary.extraction_yield_fraction);
}

TEST_CASE("a bean changes run identity but not recipe physics",
          "[flavor][integration][artifacts]") {
    const Recipe plain = testing::baseline_recipe();
    const Recipe flavoured = testing::hologram_recipe();
    const Recipe other = with_bean("continental-dark-roast");

    // A recipe naming a bean is a different document reporting a different
    // flavour, so it must not collide with the beanless run's artifacts.
    REQUIRE(artifact_io::recipe_hash(flavoured) != artifact_io::recipe_hash(plain));
    REQUIRE(artifact_io::recipe_hash(flavoured) != artifact_io::recipe_hash(other));

    ShotResult a = run(flavoured);
    ShotResult b = run(other);
    artifact_io::stamp_manifest(a, flavoured, testing::baseline_coefficients(), {});
    artifact_io::stamp_manifest(b, other, testing::baseline_coefficients(), {});
    REQUIRE(a.manifest.result_hash != b.manifest.result_hash);
    REQUIRE(a.manifest.run_id != b.manifest.run_id);
}

TEST_CASE("the flavour block is serialized only alongside a bean",
          "[flavor][integration][artifacts]") {
    const ShotResult plain = run(testing::baseline_recipe());
    const ShotResult flavoured = run(testing::hologram_recipe());

    REQUIRE_FALSE(nlohmann::json::parse(artifact_io::dump_result_json(plain)).contains("flavor"));
    REQUIRE_FALSE(nlohmann::json::parse(artifact_io::dump_summary_json(plain)).contains("flavor"));

    const nlohmann::json document = nlohmann::json::parse(artifact_io::dump_result_json(flavoured));
    REQUIRE(document.contains("flavor"));
    const nlohmann::json& flavor = document.at("flavor");
    REQUIRE(flavor.at("bean_id") == "counter-culture-hologram");
    REQUIRE(flavor.at("flavor_model_version") == std::string(version::kFlavorModel));
    REQUIRE(flavor.at("axes").size() == kSensoryAxisCount);
    REQUIRE(flavor.at("composition_percent").size() == kSoluteClassCount);
    REQUIRE(flavor.at("series").size() == flavoured.samples.size());

    // Determinism of the series, which deliberately does not enter result_hash.
    REQUIRE(artifact_io::dump_flavor_series_csv(flavoured) ==
            artifact_io::dump_flavor_series_csv(run(testing::hologram_recipe())));
}
