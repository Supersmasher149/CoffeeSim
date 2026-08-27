#pragma once
#include <filesystem>

#include "espressolab/artifact_io.hpp"
#include "espressolab/types.hpp"
#include "espressolab/units.hpp"

// Golden recipes and fixed coefficients shared by the test binary. Section 3.1:
// fixtures never become production defaults, so they live here rather than in
// assets/.
namespace espressolab::testing {

inline std::filesystem::path asset_dir() { return ESPRESSOLAB_ASSET_DIR; }
inline std::filesystem::path fixture_dir() { return ESPRESSOLAB_FIXTURE_DIR; }
inline std::filesystem::path reference_dir() { return ESPRESSOLAB_REFERENCE_DIR; }

inline Recipe baseline_recipe() {
    return artifact_io::load_recipe_file(asset_dir() / "recipes" / "baseline.json");
}

inline ModelCoefficients baseline_coefficients() {
    return artifact_io::load_coefficients_file(asset_dir() / "coefficients" / "default-v1.json");
}

// A puck fine enough to choke: used to prove that a stalled shot degrades into
// a warning rather than a numerical failure (14.2).
inline Recipe choked_recipe() {
    Recipe recipe = baseline_recipe();
    recipe.name = "choked";
    recipe.particle_diameter_m = units::microns_to_m(155.0);
    recipe.particle_spread_factor = 0.95;
    return recipe;
}

inline Recipe gusher_recipe() {
    Recipe recipe = baseline_recipe();
    recipe.name = "gusher";
    recipe.particle_diameter_m = units::microns_to_m(750.0);
    recipe.particle_spread_factor = 0.15;
    recipe.pressure_pa = PiecewiseLinearProfile::constant(units::bar_to_pa(9.0));
    return recipe;
}

inline Recipe channelled_recipe() {
    return artifact_io::load_recipe_file(asset_dir() / "recipes" / "channelled.json");
}

}  // namespace espressolab::testing
