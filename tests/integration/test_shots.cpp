#include <algorithm>
#include <catch_amalgamated.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/artifact_io.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;

namespace {

bool has_hard_warning(const ShotResult& result) {
    return std::any_of(result.warnings.begin(), result.warnings.end(), [](const auto& w) {
        return w.severity == WarningSeverity::hard;
    });
}

}  // namespace

// Section 14.2.
TEST_CASE("the baseline recipe stops on its beverage-mass target", "[integration]") {
    const Recipe recipe = testing::baseline_recipe();
    const ShotResult result = Simulator().run(recipe, testing::baseline_coefficients());

    REQUIRE(result.summary.termination == TerminationReason::target_mass_reached);
    REQUIRE(result.summary.target_mass_reached);
    REQUIRE_FALSE(has_hard_warning(result));

    REQUIRE(units::kg_to_grams(result.summary.beverage_mass_kg) == Catch::Approx(36.0).margin(0.3));
    REQUIRE(result.summary.elapsed_time_s < recipe.maximum_time_s);

    // Sanity bands, not accuracy claims: an espresso-shaped result rather than
    // a validated prediction (8.5).
    REQUIRE(result.summary.elapsed_time_s > 15.0);
    REQUIRE(result.summary.tds_fraction > 0.04);
    REQUIRE(result.summary.tds_fraction < 0.16);
    REQUIRE(result.summary.extraction_yield_fraction > 0.10);
    REQUIRE(result.summary.extraction_yield_fraction < 0.30);
    REQUIRE(result.summary.brew_ratio == Catch::Approx(2.0).margin(0.05));
}

TEST_CASE("all required series contain finite values", "[integration]") {
    // FR-02.
    const ShotResult result =
        Simulator().run(testing::baseline_recipe(), testing::baseline_coefficients());
    REQUIRE(result.samples.size() > 10);

    for (const auto& s : result.samples) {
        REQUIRE(std::isfinite(s.time_s));
        REQUIRE(std::isfinite(s.pressure_pa));
        REQUIRE(std::isfinite(s.inlet_temperature_k));
        REQUIRE(std::isfinite(s.puck_temperature_k));
        REQUIRE(std::isfinite(s.flow_m3_s));
        REQUIRE(std::isfinite(s.beverage_mass_kg));
        REQUIRE(std::isfinite(s.tds_fraction));
        REQUIRE(std::isfinite(s.extraction_yield_fraction));
    }
}

TEST_CASE("a very coarse puck runs fast and weak", "[integration]") {
    const ShotResult result =
        Simulator().run(testing::gusher_recipe(), testing::baseline_coefficients());

    REQUIRE(result.summary.termination == TerminationReason::target_mass_reached);
    REQUIRE(result.summary.elapsed_time_s < 12.0);
    REQUIRE(result.summary.tds_fraction < 0.06);
    REQUIRE(std::isfinite(result.summary.tds_fraction));
}

TEST_CASE("a choked puck stalls with a warning instead of failing numerically",
          "[integration]") {
    const ShotResult result =
        Simulator().run(testing::choked_recipe(), testing::baseline_coefficients());

    REQUIRE(result.summary.termination == TerminationReason::time_limit_reached);
    REQUIRE(result.summary.peak_flow_m3_s >= 0.0);
    REQUIRE(result.summary.peak_flow_m3_s < 1.0e-6);
    REQUIRE_FALSE(result.warnings.empty());  // never silent (FR-08)
    for (const auto& s : result.samples) REQUIRE(std::isfinite(s.flow_m3_s));
}

TEST_CASE("pre-infusion delays beverage production", "[integration]") {
    const ModelCoefficients coeff = testing::baseline_coefficients();
    const Recipe pre_infusion =
        artifact_io::load_recipe_file(testing::asset_dir() / "recipes" / "pre-infusion.json");
    const Recipe immediate = artifact_io::load_recipe_file(testing::asset_dir() / "recipes" /
                                                           "immediate-pressure.json");

    const ShotResult slow = Simulator().run(pre_infusion, coeff);
    const ShotResult fast = Simulator().run(immediate, coeff);

    const auto first_drop_time = [](const ShotResult& result) {
        for (const auto& s : result.samples) {
            if (s.beverage_mass_kg > 1.0e-6) return s.time_s;
        }
        return std::numeric_limits<double>::infinity();
    };

    REQUIRE(first_drop_time(slow) > first_drop_time(fast));
    REQUIRE(slow.summary.elapsed_time_s > fast.summary.elapsed_time_s);
}

TEST_CASE("the same inputs reproduce the same result hash", "[integration]") {
    // FR-09.
    const Recipe recipe = testing::baseline_recipe();
    const ModelCoefficients coeff = testing::baseline_coefficients();
    const SimulationConfig config;

    ShotResult first = Simulator().run(recipe, coeff, config);
    ShotResult second = Simulator().run(recipe, coeff, config);
    artifact_io::stamp_manifest(first, recipe, coeff, config);
    artifact_io::stamp_manifest(second, recipe, coeff, config);

    REQUIRE(first.manifest.result_hash == second.manifest.result_hash);
    REQUIRE(first.manifest.run_id == second.manifest.run_id);

    // A different grind must produce a different hash.
    Recipe altered = recipe;
    altered.particle_diameter_m = units::microns_to_m(360.0);
    ShotResult third = Simulator().run(altered, coeff, config);
    artifact_io::stamp_manifest(third, altered, coeff, config);
    REQUIRE(third.manifest.result_hash != first.manifest.result_hash);
}

TEST_CASE("a 60 second shot at 100 Hz simulates well inside the budget",
          "[integration][performance]") {
    // Non-functional target from 2.1: under 20 ms in a release build. The bound
    // here is loose enough not to flake on a shared CI machine but tight enough
    // to catch an order-of-magnitude regression.
    Recipe recipe = testing::baseline_recipe();
    recipe.maximum_time_s = 60.0;
    recipe.target_beverage_mass_kg.reset();

    const auto started = std::chrono::steady_clock::now();
    const ShotResult result = Simulator().run(recipe, testing::baseline_coefficients());
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE(result.diagnostics.step_count >= 6000);
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 200);
}
