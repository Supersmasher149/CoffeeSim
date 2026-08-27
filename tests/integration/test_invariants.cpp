#include <catch_amalgamated.hpp>
#include <cmath>
#include <random>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;

// Section 14.3: property and invariant tests.
TEST_CASE("beverage mass and extraction yield never go backwards", "[invariants]") {
    const ShotResult result =
        Simulator().run(testing::baseline_recipe(), testing::baseline_coefficients());

    double previous_mass = -1.0;
    double previous_yield = -1.0;
    for (const auto& s : result.samples) {
        REQUIRE(s.beverage_mass_kg >= previous_mass - 1.0e-15);
        REQUIRE(s.extraction_yield_fraction >= previous_yield - 1.0e-15);
        REQUIRE(s.beverage_mass_kg >= 0.0);
        REQUIRE(s.tds_fraction >= 0.0);
        REQUIRE(s.tds_fraction <= 1.0);
        REQUIRE(s.extraction_yield_fraction >= 0.0);
        REQUIRE(s.saturation >= 0.0);
        REQUIRE(s.saturation <= 1.0);
        previous_mass = s.beverage_mass_kg;
        previous_yield = s.extraction_yield_fraction;
    }
}

TEST_CASE("extraction yield cannot exceed the extractable fraction", "[invariants]") {
    const ModelCoefficients coeff = testing::baseline_coefficients();
    Recipe recipe = testing::baseline_recipe();
    recipe.maximum_time_s = 60.0;
    recipe.target_beverage_mass_kg.reset();  // run to the time limit

    const ShotResult result = Simulator().run(recipe, coeff);
    REQUIRE(result.summary.extraction_yield_fraction <= coeff.extractable_solids_fraction + 1.0e-12);
}

TEST_CASE("mass balances close to within tolerance", "[invariants]") {
    // 9.3: water in = retained water + beverage out + residual, and solids
    // extracted = pore solids + cup solids + residual.
    for (const Recipe& recipe :
         {testing::baseline_recipe(), testing::gusher_recipe(), testing::choked_recipe()}) {
        const ShotResult result = Simulator().run(recipe, testing::baseline_coefficients());
        INFO("recipe: " << recipe.name);
        REQUIRE(std::abs(result.diagnostics.water_mass_residual_kg) < 1.0e-9);
        REQUIRE(std::abs(result.diagnostics.solids_mass_residual_kg) < 1.0e-9);
    }
}

// The Level 2 residuals are sums across regional balances, so a per-region
// bookkeeping slip cancels in the aggregate only by luck. An eight-way split
// with unequal permeability is where such a slip shows up.
TEST_CASE("mass balances close across parallel regions", "[invariants][regions]") {
    Recipe eight = testing::baseline_recipe();
    eight.name = "eight unequal regions";
    eight.parallel_regions.clear();
    for (int i = 0; i < 8; ++i) {
        eight.parallel_regions.push_back({0.125, 0.4 + 0.3 * static_cast<double>(i)});
    }
    REQUIRE(eight.validate().ok());

    for (const Recipe& recipe : {testing::channelled_recipe(), eight}) {
        const ShotResult result = Simulator().run(recipe, testing::baseline_coefficients());
        INFO("recipe: " << recipe.name);
        REQUIRE(std::abs(result.diagnostics.water_mass_residual_kg) < 1.0e-9);
        REQUIRE(std::abs(result.diagnostics.solids_mass_residual_kg) < 1.0e-9);

        // Every region reports a share of one shot's flow.
        double flow_fraction_total = 0.0;
        double regional_beverage_kg = 0.0;
        for (const RegionSummary& region : result.regions) {
            REQUIRE(std::isfinite(region.flow_fraction));
            REQUIRE(region.flow_fraction >= 0.0);
            REQUIRE(region.flow_fraction <= 1.0 + 1.0e-9);
            flow_fraction_total += region.flow_fraction;
            regional_beverage_kg += region.beverage_mass_kg;
        }
        REQUIRE(flow_fraction_total == Catch::Approx(1.0).margin(1.0e-9));
        REQUIRE(regional_beverage_kg == Catch::Approx(result.summary.beverage_mass_kg));
    }
}

TEST_CASE("zero pressure produces no flow and no beverage", "[invariants]") {
    Recipe recipe = testing::baseline_recipe();
    recipe.pressure_pa = PiecewiseLinearProfile::constant(0.0);
    recipe.target_beverage_mass_kg.reset();

    const ShotResult result = Simulator().run(recipe, testing::baseline_coefficients());
    REQUIRE(result.summary.termination == TerminationReason::time_limit_reached);
    REQUIRE(result.summary.beverage_mass_kg == 0.0);
    REQUIRE(result.summary.peak_flow_m3_s == 0.0);
    REQUIRE(result.summary.extraction_yield_fraction == 0.0);
}

TEST_CASE("generated valid recipes never produce NaN or infinity", "[invariants][property]") {
    std::mt19937 rng(20260825);  // fixed seed: a failing case must be reproducible
    std::uniform_real_distribution<double> dose_g(14.0, 22.0);
    std::uniform_real_distribution<double> diameter_mm(51.0, 58.5);
    std::uniform_real_distribution<double> depth_mm(6.0, 14.0);
    std::uniform_real_distribution<double> particle_um(150.0, 800.0);
    std::uniform_real_distribution<double> spread(0.1, 1.0);
    std::uniform_real_distribution<double> temperature_c(85.0, 100.0);
    std::uniform_real_distribution<double> pressure_bar(0.0, 12.0);
    std::uniform_real_distribution<double> target_g(20.0, 80.0);

    const ModelCoefficients coeff = testing::baseline_coefficients();

    for (int i = 0; i < 200; ++i) {
        Recipe recipe;
        recipe.name = "generated";
        recipe.dose_kg = units::grams_to_kg(dose_g(rng));
        recipe.basket_diameter_m = units::mm_to_m(diameter_mm(rng));
        recipe.puck_depth_m = units::mm_to_m(depth_mm(rng));
        recipe.particle_diameter_m = units::microns_to_m(particle_um(rng));
        recipe.particle_spread_factor = spread(rng);
        recipe.maximum_time_s = 25.0;
        recipe.target_beverage_mass_kg = units::grams_to_kg(target_g(rng));
        recipe.pressure_pa = PiecewiseLinearProfile(
            {{0.0, units::bar_to_pa(pressure_bar(rng))},
             {8.0, units::bar_to_pa(pressure_bar(rng))},
             {30.0, units::bar_to_pa(pressure_bar(rng))}});
        recipe.inlet_temperature_k =
            PiecewiseLinearProfile::constant(units::celsius_to_kelvin(temperature_c(rng)));

        REQUIRE(recipe.validate().ok());
        const ShotResult result = Simulator().run(recipe, coeff);

        INFO("iteration " << i << " grind "
                          << units::m_to_microns(recipe.particle_diameter_m) << " um");
        REQUIRE(result.summary.termination != TerminationReason::numerical_failure);
        REQUIRE(result.summary.termination != TerminationReason::invalid_state);
        REQUIRE(std::isfinite(result.summary.beverage_mass_kg));
        REQUIRE(std::isfinite(result.summary.tds_fraction));
        REQUIRE(std::isfinite(result.summary.extraction_yield_fraction));
        REQUIRE(result.summary.beverage_mass_kg >= 0.0);
        REQUIRE(result.summary.extraction_yield_fraction >= 0.0);
        REQUIRE(std::abs(result.diagnostics.water_mass_residual_kg) < 1.0e-9);
    }
}

// Section 14.3 again, over the region partition itself: the solver must hold
// its bounds for any legal split, not only the hand-written fixtures.
TEST_CASE("generated region partitions stay finite and bounded", "[invariants][property][regions]") {
    std::mt19937 rng(20260826);  // fixed seed: a failing partition must be reproducible
    std::uniform_int_distribution<int> region_count(1, 8);
    std::uniform_real_distribution<double> weight(0.2, 1.0);
    std::uniform_real_distribution<double> permeability(0.2, 6.0);
    std::uniform_real_distribution<double> particle_um(200.0, 600.0);

    const ModelCoefficients coeff = testing::baseline_coefficients();

    for (int i = 0; i < 60; ++i) {
        Recipe recipe = testing::baseline_recipe();
        recipe.name = "generated partition";
        recipe.particle_diameter_m = units::microns_to_m(particle_um(rng));
        recipe.maximum_time_s = 30.0;

        const int count = region_count(rng);
        std::vector<double> weights(static_cast<std::size_t>(count));
        double total = 0.0;
        for (double& w : weights) {
            w = weight(rng);
            total += w;
        }
        recipe.parallel_regions.clear();
        double assigned = 0.0;
        for (int r = 0; r < count; ++r) {
            // The last fraction takes the remainder so the fractions sum to
            // exactly one rather than to one within rounding.
            const double fraction =
                r == count - 1 ? 1.0 - assigned : weights[static_cast<std::size_t>(r)] / total;
            assigned += fraction;
            recipe.parallel_regions.push_back({fraction, permeability(rng)});
        }

        INFO("iteration " << i << " with " << count << " regions");
        REQUIRE(recipe.validate().ok());

        const ShotResult result = Simulator().run(recipe, coeff);
        REQUIRE(result.summary.termination != TerminationReason::numerical_failure);
        REQUIRE(result.summary.termination != TerminationReason::invalid_state);
        REQUIRE(result.regions.size() == static_cast<std::size_t>(count));
        REQUIRE_FALSE(result.samples.empty());  // the scan below is vacuous on an empty series
        REQUIRE(std::abs(result.diagnostics.water_mass_residual_kg) < 1.0e-9);
        REQUIRE(std::abs(result.diagnostics.solids_mass_residual_kg) < 1.0e-9);

        // Scanned rather than asserted per sample: one REQUIRE per run keeps the
        // suite's assertion count meaningful, and the index says where to look.
        std::size_t bad_sample = result.samples.size();
        for (std::size_t s = 0; s < result.samples.size(); ++s) {
            const ShotSample& sample = result.samples[s];
            if (!std::isfinite(sample.saturation) || sample.saturation < 0.0 ||
                sample.saturation > 1.0 + 1.0e-9 || !std::isfinite(sample.flow_m3_s) ||
                !std::isfinite(sample.puck_temperature_k) ||
                !std::isfinite(sample.beverage_mass_kg)) {
                bad_sample = s;
                break;
            }
        }
        INFO("first nonfinite or out-of-bounds sample: " << bad_sample);
        REQUIRE(bad_sample == result.samples.size());
        for (const RegionSummary& region : result.regions) {
            REQUIRE(std::isfinite(region.beverage_mass_kg));
            REQUIRE(std::isfinite(region.tds_fraction));
            REQUIRE(std::isfinite(region.extraction_yield_fraction));
            REQUIRE(region.extraction_yield_fraction >= 0.0);
        }
    }
}
