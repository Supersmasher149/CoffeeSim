#include <cmath>
#include <string>

#include <catch_amalgamated.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/grinder.hpp"
#include "espressolab/grinder_io.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;

namespace {

GrinderSpec spec_at_gap(double gap_um) {
    GrinderSpec spec;
    spec.burr_gap_um = gap_um;
    return spec;
}

double mass_below(const GrindDistribution& distribution, double um) {
    double total = 0.0;
    for (const GrindBin& bin : distribution.bins) {
        if (units::m_to_microns(bin.diameter_m) < um) total += bin.mass_fraction;
    }
    return total;
}

}  // namespace

// ---------------------------------------------------------------------------
// The population balance's own arithmetic
// ---------------------------------------------------------------------------
TEST_CASE("breakage conserves mass exactly", "[grind_sim][verification]") {
    // Nothing is created or destroyed by breaking a particle, so every pass
    // must leave unit mass. This is the solver checked against the equation it
    // says it solves -- verification, not validation.
    for (double gap : {150.0, 400.0, 900.0}) {
        const GrinderResult result = grind(spec_at_gap(gap));
        INFO("burr gap " << gap << " um");
        REQUIRE(result.mass_balance_residual < 1.0e-12);

        double emitted = 0.0;
        for (const GrindBin& bin : result.distribution.bins) emitted += bin.mass_fraction;
        REQUIRE(emitted == Catch::Approx(1.0).epsilon(1e-12));
    }
}

TEST_CASE("the emitted distribution is one a recipe can carry", "[grind_sim]") {
    const GrinderResult result = grind(spec_at_gap(700.0));
    REQUIRE(result.distribution.validate().ok());
    REQUIRE(result.distribution.bins.size() >= kMinGrindBins);
    REQUIRE(result.distribution.bins.size() <= kMaxGrindBins);
    // Strictly increasing diameters, which the distribution contract requires.
    for (std::size_t i = 1; i < result.distribution.bins.size(); ++i) {
        REQUIRE(result.distribution.bins[i].diameter_m >
                result.distribution.bins[i - 1].diameter_m);
    }
}

// ---------------------------------------------------------------------------
// Physical behaviour the model should show
// ---------------------------------------------------------------------------
TEST_CASE("a finer burr gap gives a finer grind", "[grind_sim]") {
    double previous_d32 = 0.0;
    for (double gap : {200.0, 300.0, 450.0, 700.0, 1000.0}) {
        const GrinderResult result = grind(spec_at_gap(gap));
        INFO("burr gap " << gap << " um -> d32 " << result.sauter_mean_diameter_um);
        REQUIRE(result.sauter_mean_diameter_um > previous_d32);
        previous_d32 = result.sauter_mean_diameter_um;
    }
}

TEST_CASE("the coarse mode tracks the burr gap", "[grind_sim]") {
    // Particles at or below the gap have left the grinding zone, so the mass
    // peak should sit near it rather than grinding away toward the fines.
    for (double gap : {400.0, 700.0}) {
        const GrinderResult result = grind(spec_at_gap(gap));
        double peak_um = 0.0;
        double peak_mass = 0.0;
        for (const GrindBin& bin : result.distribution.bins) {
            if (bin.mass_fraction > peak_mass) {
                peak_mass = bin.mass_fraction;
                peak_um = units::m_to_microns(bin.diameter_m);
            }
        }
        INFO("gap " << gap << " um, peak at " << peak_um << " um");
        REQUIRE(peak_um > gap * 0.5);
        REQUIRE(peak_um < gap * 2.0);
    }
}

TEST_CASE("the distribution is bimodal, not merely broad", "[grind_sim]") {
    // The cell-wall mode is what a single log-normal cannot express: a distinct
    // spike at the fines size, separated from the coarse mode by a trough.
    GrinderSpec spec = spec_at_gap(700.0);
    const GrinderResult result = grind(spec);
    REQUIRE(result.distribution.bins.size() > 4);

    const double fines_mass = result.distribution.bins.front().mass_fraction;
    const double trough_mass = result.distribution.bins.at(1).mass_fraction;
    // A spike: the fines bin carries more mass than the bin just above it.
    REQUIRE(fines_mass > trough_mass);
    // And it is genuinely the cell-wall mode, at the size the spec named.
    REQUIRE(units::m_to_microns(result.distribution.bins.front().diameter_m) ==
            Catch::Approx(spec.fines_diameter_um));
}

TEST_CASE("fines yield controls the fines mass, and zero yield removes it",
          "[grind_sim]") {
    GrinderSpec none = spec_at_gap(700.0);
    none.fines_yield = 0.0;
    GrinderSpec some = spec_at_gap(700.0);
    some.fines_yield = 0.02;

    const GrinderResult without = grind(none);
    const GrinderResult with = grind(some);

    REQUIRE(without.cumulative_fines_fraction == 0.0);
    REQUIRE(with.cumulative_fines_fraction > without.cumulative_fines_fraction);
    REQUIRE(mass_below(with.distribution, 60.0) > mass_below(without.distribution, 60.0));
    // Fines drag the surface-weighted mean down hard for their mass share.
    REQUIRE(with.sauter_mean_diameter_um < without.sauter_mean_diameter_um);
}

TEST_CASE("more passes grind finer, with diminishing return", "[grind_sim]") {
    const double one = grind([] {
        GrinderSpec s = spec_at_gap(700.0);
        s.passes = 1;
        return s;
    }()).sauter_mean_diameter_um;
    const double many = grind([] {
        GrinderSpec s = spec_at_gap(700.0);
        s.passes = 40;
        return s;
    }()).sauter_mean_diameter_um;
    REQUIRE(many < one);
    // The burr classification means it converges rather than grinding to dust.
    REQUIRE(many > 100.0);
}

// ---------------------------------------------------------------------------
// Determinism and validation
// ---------------------------------------------------------------------------
TEST_CASE("the same spec always gives the same distribution", "[grind_sim]") {
    // No RNG anywhere and a fixed evaluation order, matching the project's
    // reproducibility contract.
    const GrinderSpec spec = spec_at_gap(550.0);
    const GrinderResult a = grind(spec);
    const GrinderResult b = grind(spec);
    REQUIRE(a.distribution.bins.size() == b.distribution.bins.size());
    for (std::size_t i = 0; i < a.distribution.bins.size(); ++i) {
        REQUIRE(a.distribution.bins[i].diameter_m == b.distribution.bins[i].diameter_m);
        REQUIRE(a.distribution.bins[i].mass_fraction == b.distribution.bins[i].mass_fraction);
    }
    REQUIRE(grinder_io::dump_result_json(spec, a) == grinder_io::dump_result_json(spec, b));
}

TEST_CASE("a nonphysical grinder spec is rejected", "[grind_sim][units]") {
    SECTION("beans must be larger than the gap") {
        GrinderSpec spec = spec_at_gap(1400.0);
        spec.bean_diameter_um = 2500.0;
        REQUIRE(spec.validate().ok());  // both fields individually in range
        spec.burr_gap_um = 1400.0;
        spec.bean_diameter_um = 1300.0;  // now below the gap
        REQUIRE_FALSE(spec.validate().ok());
    }
    SECTION("fines must be finer than the gap") {
        GrinderSpec spec = spec_at_gap(150.0);
        spec.fines_diameter_um = 180.0;
        REQUIRE_FALSE(spec.validate().ok());
    }
    SECTION("grind() throws rather than returning a garbage distribution") {
        GrinderSpec spec = spec_at_gap(400.0);
        spec.passes = 0;
        REQUIRE_THROWS_AS(grind(spec), InvalidInputError);
    }
}

// ---------------------------------------------------------------------------
// The seam with phase 1: a generated distribution must actually be usable
// ---------------------------------------------------------------------------
TEST_CASE("a generated distribution round-trips into a recipe", "[grind_sim][integration]") {
    const GrinderSpec spec = grinder_io::load_spec_file(testing::asset_dir() / "grinders" /
                                                        "burr-baseline.json");
    const GrinderResult result = grind(spec);

    Recipe recipe = testing::baseline_recipe();
    recipe.grind = result.distribution;
    recipe.particle_diameter_m = result.distribution.sauter_mean_diameter_m();
    recipe.particle_spread_factor = result.distribution.equivalent_spread_factor();

    INFO("d32 = " << result.sauter_mean_diameter_um << " um");
    REQUIRE(recipe.validate().ok());

    // And it survives serialization the way any recipe distribution must.
    const std::string once = artifact_io::dump_recipe_json(recipe, -1);
    const std::string twice =
        artifact_io::dump_recipe_json(artifact_io::load_recipe_json(once), -1);
    REQUIRE(once == twice);
}

TEST_CASE("a spec file round-trips through the loader", "[grind_sim][artifacts]") {
    const GrinderSpec spec = grinder_io::load_spec_file(testing::asset_dir() / "grinders" /
                                                        "burr-coarse.json");
    REQUIRE(spec.burr_gap_um == Catch::Approx(1000.0));
    REQUIRE(spec.name == "58 mm flat burr, coarser setting");

    // An empty document is legal: every field defaults to the compiled-in spec.
    const GrinderSpec defaults = grinder_io::load_spec_json("{}");
    REQUIRE(defaults.burr_gap_um == GrinderSpec{}.burr_gap_um);
    REQUIRE_THROWS_AS(grinder_io::load_spec_json("{ not json"), InvalidInputError);
    REQUIRE_THROWS_AS(grinder_io::load_spec_json(R"({"passes": 1.5})"), InvalidInputError);
}
