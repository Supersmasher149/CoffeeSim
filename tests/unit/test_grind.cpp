#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include <catch_amalgamated.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/artifact_io.hpp"
#include "espressolab/experiment.hpp"
#include "espressolab/grind.hpp"
#include "espressolab/puck.hpp"
#include "espressolab/simulator.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;

namespace {

GrindDistribution monodisperse(double diameter_um) {
    return GrindDistribution{{{units::microns_to_m(diameter_um), 1.0}}};
}

// A realistic bimodal grind: a small mass fraction of cell-wall fines under a
// dominant coarse fragment mode. Mirrors assets/recipes/psd-bimodal.json.
GrindDistribution bimodal() {
    GrindDistribution grind;
    const std::vector<std::pair<double, double>> bins = {
        {30.0, 0.015}, {60.0, 0.012}, {120.0, 0.013}, {250.0, 0.100},
        {400.0, 0.280}, {550.0, 0.300}, {700.0, 0.200}, {900.0, 0.080}};
    for (const auto& [um, fraction] : bins) {
        grind.bins.push_back({units::microns_to_m(um), fraction});
    }
    return grind;
}

ShotResult run(const Recipe& recipe, const ModelCoefficients& coeff) {
    return Simulator().run(recipe, coeff);
}

}  // namespace

// ---------------------------------------------------------------------------
// The guarantee that makes this change safe to land: a recipe that carries no
// distribution must hash exactly as it did before the PSD path existed. These
// are the values recorded from the build immediately preceding it.
// ---------------------------------------------------------------------------
TEST_CASE("scalar recipes keep their pre-PSD hashes", "[grind][artifacts]") {
    const std::map<std::string, std::string> golden_recipe_hashes = {
        {"axial-resolved.json", "2e9673ba2145030279b6a12fdd118b7e49bd65a855db37018c9ecdaca31edd2e"},
        {"baseline.json", "f744a2ed317ba0cfca42b9bd8940aa049a79f530403923a9f31133d0b858e3ab"},
        {"breville-barista-touch-bes880-baseline.json",
         "636ee18a075014ce883e4e51da35d78c3fa4f1ef641181540ed61d9e1050da5c"},
        {"breville-barista-touch-bes880-coarse.json",
         "c33a7da0322c06538b674dce44d64272f7c2cf38496c43e87241a1b91dae956a"},
        {"breville-barista-touch-bes880-fine.json",
         "5ebd5b2376175f3ac57c89903c371b503c885abce91cd271bdb2441277f8e393"},
        {"channelled.json", "f7bcc9c53d02baacbfdcfac156a358191dedd356c47c02680ce6915670b1fe60"},
        {"coarse.json", "e69f8eb598845dea38004a58e62895e473d71376fed445651ab628e95ddff429"},
        {"fine.json", "761fc4db5f9c15a233a86acf1af726e7d88e92e4b879aac24d6824fdb03d98a0"},
        {"gaggia-classic-e24-baseline.json",
         "513627c7d62b74d71a05d8a47db43b4743dc1f8d9894478c8e4fe0888e54c084"},
        {"gaggia-classic-e24-coarse.json",
         "c5f772806e2cd277612ede0d2cfe797d6755654c4a0ad309e0b972b14e293b44"},
        {"gaggia-classic-e24-fine.json",
         "c454475345a75fbb199d58292698262345797b5e12467feb0a8c859e59e795f5"},
        {"immediate-pressure.json",
         "f2420207c9cb77cd2a09c8abcc3ebbe827fc881c61425b514edaf5d1466df892"},
        {"pre-infusion.json", "0aa58d6cf831b9f9aa95c519efe962b8cb48147c76f699a959aa7032bfc82a58"},
    };

    for (const auto& [name, expected] : golden_recipe_hashes) {
        const Recipe recipe =
            artifact_io::load_recipe_file(testing::asset_dir() / "recipes" / name);
        INFO("recipe " << name);
        REQUIRE_FALSE(recipe.grind.has_value());
        REQUIRE(artifact_io::recipe_hash(recipe) == expected);
    }
}

TEST_CASE("scalar recipes keep their pre-PSD result hashes", "[grind][artifacts]") {
    // The raw result_hash is a digest over the full sample series, so it folds
    // in every std::pow/std::exp/std::log call the solver makes across every
    // step of every cell. Two libm implementations are free to differ by a
    // ULP on any of those and, via the hash's avalanche effect, land on a
    // completely different string with no change in the physics -- exactly
    // the "no exact-snapshot golden test" the reproducibility section of
    // CLAUDE.md rules out. What actually needs guarding -- that the PSD path
    // did not change a grind-less recipe's outcome -- is checked here the way
    // the rest of this file does it: comparing the physically meaningful
    // summary fields with Catch::Approx, not an opaque hash.
    struct Expected {
        double beverage_mass_kg;
        double extraction_yield_fraction;
        double tds_fraction;
        double elapsed_time_s;
    };
    const std::map<std::string, Expected> golden_summaries = {
        {"baseline.json", {0.03601492034, 0.1818051546, 0.09086491797, 29.03}},
        {"fine.json", {0.01606784426, 0.1249365212, 0.1399601181, 45.0}},
        {"coarse.json", {0.03600108724, 0.04707440552, 0.02353649194, 4.68}},
        {"channelled.json", {0.03600993362, 0.06259956285, 0.03129114714, 23.62}},
        {"axial-resolved.json", {0.03602487273, 0.2192984647, 0.109573527, 31.67}},
        {"pre-infusion.json", {0.03600527466, 0.1905055959, 0.09523884371, 34.05}},
    };
    const ModelCoefficients coeff = testing::baseline_coefficients();
    for (const auto& [name, expected] : golden_summaries) {
        const Recipe recipe =
            artifact_io::load_recipe_file(testing::asset_dir() / "recipes" / name);
        INFO("recipe " << name);
        const ShotResult result = run(recipe, coeff);
        REQUIRE(result.summary.beverage_mass_kg ==
                Catch::Approx(expected.beverage_mass_kg).epsilon(1e-3));
        REQUIRE(result.summary.extraction_yield_fraction ==
                Catch::Approx(expected.extraction_yield_fraction).epsilon(1e-3));
        REQUIRE(result.summary.tds_fraction == Catch::Approx(expected.tds_fraction).epsilon(1e-3));
        REQUIRE(result.summary.elapsed_time_s ==
                Catch::Approx(expected.elapsed_time_s).epsilon(0.02));
    }
}

// ---------------------------------------------------------------------------
// d32 and the derived spread
// ---------------------------------------------------------------------------
TEST_CASE("a monodisperse distribution returns its own diameter", "[grind]") {
    for (double um : {150.0, 350.0, 800.0}) {
        const GrindDistribution grind = monodisperse(um);
        REQUIRE(units::m_to_microns(grind.sauter_mean_diameter_m()) == Catch::Approx(um).epsilon(0));
        REQUIRE(grind.geometric_std_dev() == 1.0);          // exactly, not approximately
        REQUIRE(grind.equivalent_spread_factor() == 0.1);   // the narrow end of the scale
    }
}

TEST_CASE("d32 matches the hand-computed harmonic form", "[grind]") {
    // d32 = 1 / sum(w_i / d_i): half the mass at 200 um, half at 600 um gives
    // 1 / (0.5/200 + 0.5/600) = 300 um, not the arithmetic mean of 400 um.
    GrindDistribution grind;
    grind.bins = {{units::microns_to_m(200.0), 0.5}, {units::microns_to_m(600.0), 0.5}};
    REQUIRE(units::m_to_microns(grind.sauter_mean_diameter_m()) == Catch::Approx(300.0));
    REQUIRE(grind.sauter_mean_diameter_m() < units::microns_to_m(400.0));
}

TEST_CASE("d32 is dragged down by a small mass of fines", "[grind]") {
    // The physical point of carrying a distribution: 4% of the mass below
    // 120 um moves the hydraulic length scale far more than its mass share.
    const GrindDistribution grind = bimodal();
    double mass_weighted_mean_m = 0.0;
    for (const GrindBin& bin : grind.bins) mass_weighted_mean_m += bin.mass_fraction * bin.diameter_m;
    REQUIRE(grind.sauter_mean_diameter_m() < mass_weighted_mean_m);
    REQUIRE(units::m_to_microns(grind.sauter_mean_diameter_m()) == Catch::Approx(353.56).margin(0.1));
}

TEST_CASE("the derived spread widens with the distribution", "[grind]") {
    GrindDistribution narrow;
    narrow.bins = {{units::microns_to_m(340.0), 0.5}, {units::microns_to_m(360.0), 0.5}};
    const GrindDistribution broad = bimodal();
    REQUIRE(narrow.geometric_std_dev() < broad.geometric_std_dev());
    REQUIRE(narrow.equivalent_spread_factor() < broad.equivalent_spread_factor());
    for (const GrindDistribution& g : {narrow, broad}) {
        REQUIRE(g.equivalent_spread_factor() >= 0.1);
        REQUIRE(g.equivalent_spread_factor() <= 1.0);
    }
}

// ---------------------------------------------------------------------------
// Consistency with the scalar physics
// ---------------------------------------------------------------------------
TEST_CASE("permeability at d32 equals the scalar call", "[grind][permeability]") {
    const GrindDistribution grind = bimodal();
    const double d32 = grind.sauter_mean_diameter_m();
    REQUIRE(kozeny_carman_permeability(d32, 0.42, 4.0e6) ==
            kozeny_carman_permeability(grind.sauter_mean_diameter_m(), 0.42, 4.0e6));
    // And it is a real reduction against the coarse mode the eye would pick.
    REQUIRE(kozeny_carman_permeability(d32, 0.42, 4.0e6) <
            kozeny_carman_permeability(units::microns_to_m(550.0), 0.42, 4.0e6));
}

TEST_CASE("a single-bin PSD reproduces the scalar shot exactly", "[grind][integration]") {
    // The strongest available equivalence check: express the scalar case as a
    // one-bin distribution and the whole pipeline -- derivation, solver, hash --
    // must land on the identical result, not merely a close one.
    const ModelCoefficients coeff = testing::baseline_coefficients();

    Recipe scalar = testing::baseline_recipe();
    scalar.particle_diameter_m = units::microns_to_m(350.0);
    scalar.particle_spread_factor = 0.1;  // what a monodisperse PSD derives

    Recipe psd = scalar;
    psd.grind = monodisperse(350.0);
    psd.particle_diameter_m = psd.grind->sauter_mean_diameter_m();
    psd.particle_spread_factor = psd.grind->equivalent_spread_factor();

    const ShotResult a = run(scalar, coeff);
    const ShotResult b = run(psd, coeff);
    REQUIRE(b.samples.size() == a.samples.size());
    REQUIRE(b.summary.beverage_mass_kg == a.summary.beverage_mass_kg);
    REQUIRE(b.summary.extraction_yield_fraction == a.summary.extraction_yield_fraction);
    REQUIRE(b.summary.tds_fraction == a.summary.tds_fraction);
}

// ---------------------------------------------------------------------------
// The behaviour a scalar diameter structurally cannot produce
// ---------------------------------------------------------------------------
TEST_CASE("fines deplete first, so a PSD extracts less than its own d32",
          "[grind][integration]") {
    // With grind_exponent = 1 the extraction rate is proportional to 1/d, and
    // sum(w_i / d_i) is exactly 1/d32 -- so at t = 0 the distribution and a
    // scalar puck at its d32 extract at the identical rate. They can only
    // diverge as the fast bins run out, and the distribution must fall behind.
    // That divergence is the whole reason for carrying size structure.
    ModelCoefficients coeff = testing::baseline_coefficients();
    REQUIRE(coeff.grind_exponent == Catch::Approx(1.0));

    const GrindDistribution grind = bimodal();

    Recipe psd = testing::baseline_recipe();
    psd.grind = grind;
    psd.particle_diameter_m = grind.sauter_mean_diameter_m();
    psd.particle_spread_factor = grind.equivalent_spread_factor();

    Recipe scalar = psd;
    scalar.grind.reset();  // identical hydraulics, lumped extraction

    const ShotResult psd_result = run(psd, coeff);
    const ShotResult scalar_result = run(scalar, coeff);

    // Both reach the same target mass on nearly the same schedule -- the beds
    // are the same bed. They are not step-for-step identical, because dissolved
    // solids add mass and so feed back into termination.
    REQUIRE(psd_result.summary.beverage_mass_kg ==
            Catch::Approx(scalar_result.summary.beverage_mass_kg).epsilon(1e-3));
    REQUIRE(psd_result.summary.elapsed_time_s ==
            Catch::Approx(scalar_result.summary.elapsed_time_s).epsilon(0.02));

    // The claim, and the whole reason for carrying size structure: sharing a
    // d32, the distribution extracts strictly less than the lumped puck, and
    // the gap widens as its fast bins empty. A single pool has no mechanism to
    // slow down this way -- it cannot run out of fines, having none.
    //
    // Note the divergence is already visible on the first drop into the cup:
    // the 30 um bin extracts at roughly twelve times the reference rate, so it
    // is spent during pre-infusion, before anything leaves the puck at all.
    auto yield_at = [](const ShotResult& r, double time_s) {
        double best = 0.0;
        for (const ShotSample& sample : r.samples) {
            if (sample.time_s <= time_s) best = sample.extraction_yield_fraction;
        }
        return best;
    };
    const double early_psd = yield_at(psd_result, 14.0);
    const double early_scalar = yield_at(scalar_result, 14.0);
    REQUIRE(early_psd > 0.0);
    REQUIRE(early_psd < early_scalar);

    const double early_gap = 1.0 - early_psd / early_scalar;
    const double final_gap = 1.0 - psd_result.summary.extraction_yield_fraction /
                                       scalar_result.summary.extraction_yield_fraction;
    REQUIRE(final_gap > early_gap);

    REQUIRE(psd_result.summary.extraction_yield_fraction <
            scalar_result.summary.extraction_yield_fraction);
}

TEST_CASE("a size-resolved shot closes its mass balances", "[grind][invariants]") {
    const Recipe recipe =
        artifact_io::load_recipe_file(testing::asset_dir() / "recipes" / "psd-bimodal.json");
    REQUIRE(recipe.grind.has_value());
    const ShotResult result = run(recipe, testing::baseline_coefficients());
    REQUIRE(std::abs(result.diagnostics.water_mass_residual_kg) < 1.0e-12);
    REQUIRE(std::abs(result.diagnostics.solids_mass_residual_kg) < 1.0e-12);
    REQUIRE(result.summary.beverage_mass_kg > 0.0);
}

// ---------------------------------------------------------------------------
// Validation and the loader contract
// ---------------------------------------------------------------------------
TEST_CASE("a malformed distribution is rejected with a path", "[grind][units]") {
    auto issue_paths = [](const ValidationResult& result) {
        std::vector<std::string> paths;
        for (const ValidationIssue& issue : result.issues()) paths.push_back(issue.path);
        return paths;
    };

    SECTION("mass fractions must sum to 1") {
        GrindDistribution grind;
        grind.bins = {{units::microns_to_m(200.0), 0.4}, {units::microns_to_m(500.0), 0.4}};
        const ValidationResult result = grind.validate();
        REQUIRE_FALSE(result.ok());
        const auto paths = issue_paths(result);
        REQUIRE(std::find(paths.begin(), paths.end(), "recipe.puck.grind.bins") != paths.end());
    }

    SECTION("diameters must strictly increase") {
        GrindDistribution grind;
        grind.bins = {{units::microns_to_m(500.0), 0.5}, {units::microns_to_m(200.0), 0.5}};
        REQUIRE_FALSE(grind.validate().ok());
    }

    SECTION("an empty distribution is rejected") {
        REQUIRE_FALSE(GrindDistribution{}.validate().ok());
    }

    SECTION("a distribution whose d32 leaves the supported envelope is rejected") {
        // Every bin is individually legal, but together they describe a bed far
        // outside what the correlations were shaped around.
        Recipe recipe = testing::baseline_recipe();
        GrindDistribution grind;
        grind.bins = {{units::microns_to_m(10.0), 0.5}, {units::microns_to_m(20.0), 0.5}};
        REQUIRE(grind.validate().ok());
        recipe.grind = grind;
        const ValidationResult result = recipe.validate();
        REQUIRE_FALSE(result.ok());
    }
}

TEST_CASE("the two grind spellings are mutually exclusive", "[grind][artifacts]") {
    const std::string both = R"({
      "schema_version": "1.0", "name": "conflict",
      "puck": { "dose_g": 18, "basket_diameter_mm": 58, "depth_mm": 9,
                "particle_diameter_um": 350, "particle_spread_factor": 0.55,
                "grind": { "bins": [ {"diameter_um": 350, "mass_fraction": 1.0} ] } },
      "pressure_profile_bar": [[0, 9]], "temperature_profile_c": [[0, 93]],
      "stop": { "maximum_time_s": 30 } })";
    REQUIRE_THROWS_AS(artifact_io::load_recipe_json(both), artifact_io::LoadError);
}

TEST_CASE("a PSD recipe round-trips to a stable document", "[grind][artifacts]") {
    // load -> dump -> load -> dump must be a fixed point, or simply re-saving a
    // recipe would move its hash and break reproducibility.
    const Recipe once =
        artifact_io::load_recipe_file(testing::asset_dir() / "recipes" / "psd-bimodal.json");
    const std::string first = artifact_io::dump_recipe_json(once, -1);
    const Recipe twice = artifact_io::load_recipe_json(first);
    const std::string second = artifact_io::dump_recipe_json(twice, -1);
    REQUIRE(first == second);
    REQUIRE(artifact_io::recipe_hash(once) == artifact_io::recipe_hash(twice));

    // The derived scalars are not serialized: the distribution is the input.
    REQUIRE(first.find("particle_diameter_um") == std::string::npos);
    REQUIRE(first.find("particle_spread_factor") == std::string::npos);
    REQUIRE(twice.grind.has_value());
    REQUIRE(twice.grind->bins.size() == once.grind->bins.size());
}

// ---------------------------------------------------------------------------
// Sweeps over a distribution-bearing recipe
// ---------------------------------------------------------------------------
TEST_CASE("sweeping grind size scales the whole distribution", "[grind][sweep]") {
    Recipe baseline = testing::baseline_recipe();
    baseline.grind = bimodal();
    baseline.particle_diameter_m = baseline.grind->sauter_mean_diameter_m();
    baseline.particle_spread_factor = baseline.grind->equivalent_spread_factor();

    const double original_sigma = baseline.grind->geometric_std_dev();
    const Recipe swept = apply_parameter(baseline, "puck.particle_diameter_um", 450.0);

    REQUIRE(swept.grind.has_value());
    // The requested diameter is hit exactly, not approached.
    REQUIRE(units::m_to_microns(swept.grind->sauter_mean_diameter_m()) ==
            Catch::Approx(450.0).epsilon(1e-12));
    // The loader's invariant -- the scalar IS the bins' d32 -- survives a sweep.
    REQUIRE(swept.particle_diameter_m == swept.grind->sauter_mean_diameter_m());
    // Shape is preserved: a uniform scaling shifts ln(d) and leaves its
    // variance -- and therefore the spread -- alone.
    REQUIRE(swept.grind->geometric_std_dev() == Catch::Approx(original_sigma).epsilon(1e-12));
    REQUIRE(swept.particle_spread_factor == Catch::Approx(baseline.particle_spread_factor));
    // Every bin moved by the same factor, so the mass split is untouched.
    REQUIRE(swept.grind->bins.size() == baseline.grind->bins.size());
    for (std::size_t i = 0; i < swept.grind->bins.size(); ++i) {
        REQUIRE(swept.grind->bins[i].mass_fraction == baseline.grind->bins[i].mass_fraction);
    }
}

TEST_CASE("sweeping grind size on a scalar recipe is unchanged", "[grind][sweep]") {
    const Recipe baseline = testing::baseline_recipe();
    REQUIRE_FALSE(baseline.grind.has_value());
    const Recipe swept = apply_parameter(baseline, "puck.particle_diameter_um", 450.0);
    REQUIRE(units::m_to_microns(swept.particle_diameter_m) == Catch::Approx(450.0));
    REQUIRE(swept.particle_spread_factor == baseline.particle_spread_factor);
    REQUIRE_FALSE(swept.grind.has_value());
}

TEST_CASE("sweeping the spread of a distribution is refused, not guessed",
          "[grind][sweep]") {
    Recipe baseline = testing::baseline_recipe();
    baseline.grind = bimodal();
    REQUIRE_THROWS_AS(apply_parameter(baseline, "puck.particle_spread_factor", 0.8),
                      InvalidInputError);
    // The scalar spelling still sweeps normally.
    Recipe scalar = testing::baseline_recipe();
    REQUIRE(apply_parameter(scalar, "puck.particle_spread_factor", 0.8).particle_spread_factor ==
            Catch::Approx(0.8));
}

TEST_CASE("a hand-built recipe cannot run two grinds at once", "[grind][units]") {
    // The flow path reads particle_diameter_m; extraction reads the bins. Code
    // that sets a distribution without re-deriving the scalar would run one
    // grind hydraulically and a different one chemically, with both numbers
    // individually plausible. validate() must refuse it.
    Recipe recipe = testing::baseline_recipe();
    recipe.grind = bimodal();
    recipe.particle_spread_factor = recipe.grind->equivalent_spread_factor();
    recipe.particle_diameter_m = units::microns_to_m(350.0);  // stale: not the d32
    REQUIRE_FALSE(recipe.validate().ok());

    recipe.particle_diameter_m = recipe.grind->sauter_mean_diameter_m();
    REQUIRE(recipe.validate().ok());
}
