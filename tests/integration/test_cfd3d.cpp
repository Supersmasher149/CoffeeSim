#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/artifact_io.hpp"
#include "espressolab/cfd3d.hpp"
#include "espressolab/cfd3d_artifact_io.hpp"
#include "espressolab/puck.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;

namespace {

Cfd3dConfig small_config() {
    Cfd3dConfig config;
    config.mesh = {6, 6, 8};
    config.dt_s = 0.02;
    config.snapshot_interval_s = 1.0;
    return config;
}

Recipe short_recipe() {
    Recipe recipe = testing::baseline_recipe();
    recipe.maximum_time_s = 30.0;
    recipe.target_beverage_mass_kg = 0.020;
    return recipe;
}

}  // namespace

TEST_CASE("cfd3d geometry conserves the circular puck area", "[cfd3d][verification]") {
    const Cfd3dResult result =
        Cfd3dSolver().run(short_recipe(), testing::baseline_coefficients(), small_config());
    double area = 0.0;
    for (const double value : result.geometry.cell_area_xy_m2) area += value;

    REQUIRE(area == Catch::Approx(short_recipe().basket_area_m2()).epsilon(1.0e-10));
    REQUIRE(result.geometry.cell_area_xy_m2.size() == 36);
    REQUIRE(result.geometry.x_face_aperture_m.size() == 42);
    REQUIRE(result.geometry.y_face_aperture_m.size() == 42);
    REQUIRE(result.diagnostics.agglomerated_sliver_count >= 0);
}

TEST_CASE("cfd3d produces bounded deterministic fields", "[cfd3d][verification]") {
    const Recipe recipe = short_recipe();
    const ModelCoefficients coefficients = testing::baseline_coefficients();
    const Cfd3dConfig config = small_config();
    const Cfd3dResult first = Cfd3dSolver().run(recipe, coefficients, config);
    const Cfd3dResult second = Cfd3dSolver().run(recipe, coefficients, config);

    REQUIRE(first.termination == TerminationReason::target_mass_reached);
    REQUIRE(std::abs(first.diagnostics.water_mass_residual_kg) < 1.0e-9);
    REQUIRE(std::abs(first.diagnostics.solids_mass_residual_kg) < 1.0e-9);
    REQUIRE(std::abs(first.diagnostics.energy_residual_j) < 1.0e-3);
    REQUIRE(first.diagnostics.nonfinite_state_count == 0);
    REQUIRE(first.pressure_pa.values() == second.pressure_pa.values());
    REQUIRE(first.saturation.values() == second.saturation.values());
    REQUIRE(first.temperature_k.values() == second.temperature_k.values());
    for (const double value : first.saturation.values()) {
        REQUIRE(value >= 0.0);
        REQUIRE(value <= 1.0 + 1.0e-9);
    }
}

TEST_CASE("cfd3d ambient cooling is independent of axial refinement", "[cfd3d][heat][verification]") {
    Recipe recipe = testing::baseline_recipe();
    recipe.pressure_pa = PiecewiseLinearProfile::constant(0.0);
    recipe.target_beverage_mass_kg.reset();
    recipe.maximum_time_s = 10.0;

    ModelCoefficients coefficients = testing::baseline_coefficients();
    coefficients.initial_puck_temperature_k = units::celsius_to_kelvin(90.0);
    coefficients.ambient_heat_loss_w_k = 2.0;

    auto run = [&](int axial_cells) {
        Cfd3dConfig config;
        config.mesh = {6, 6, axial_cells};
        config.dt_s = 0.01;
        config.sample_interval_s = 0.5;
        config.snapshot_interval_s = 0.0;
        return Cfd3dSolver().run(recipe, coefficients, config);
    };

    const Cfd3dResult coarse = run(2);
    const Cfd3dResult refined = run(16);
    REQUIRE(coarse.samples.back().puck_temperature_k ==
            Catch::Approx(refined.samples.back().puck_temperature_k).margin(1.0e-9));
}

TEST_CASE("cfd3d hot inlet and puck preserve temperature without ambient loss",
          "[cfd3d][heat][verification]") {
    Recipe recipe = testing::baseline_recipe();
    recipe.target_beverage_mass_kg.reset();
    recipe.maximum_time_s = 10.0;

    ModelCoefficients coefficients = testing::baseline_coefficients();
    const double hot_temperature = units::celsius_to_kelvin(93.0);
    coefficients.initial_puck_temperature_k = hot_temperature;
    coefficients.ambient_heat_loss_w_k = 0.0;

    Cfd3dConfig config = small_config();
    config.snapshot_interval_s = 0.0;
    const Cfd3dResult result = Cfd3dSolver().run(recipe, coefficients, config);

    for (const ShotSample& sample : result.samples) {
        REQUIRE(sample.puck_temperature_k == Catch::Approx(hot_temperature).margin(1.0e-9));
    }
}

TEST_CASE("cfd3d reports temperature using actual thermal capacity",
          "[cfd3d][heat][verification]") {
    Recipe recipe = short_recipe();
    recipe.target_beverage_mass_kg.reset();
    recipe.maximum_time_s = 10.0;
    ModelCoefficients coefficients = testing::baseline_coefficients();
    coefficients.ambient_heat_loss_w_k = 0.0;

    Cfd3dConfig config = small_config();
    config.snapshot_interval_s = 0.0;
    config.material = Cfd3dMaterialField(config.mesh.nx, config.mesh.ny, config.mesh.nz, 1.0);
    for (int z = 0; z < config.mesh.nz; ++z) {
        for (int y = 0; y < config.mesh.ny; ++y) {
            config.material.at(config.mesh.nx - 1, y, z) = 8.0;
        }
    }

    const Cfd3dResult result = Cfd3dSolver().run(recipe, coefficients, config);
    const Cfd3dMesh& mesh = result.geometry.mesh;
    const std::size_t xy_count = static_cast<std::size_t>(mesh.nx) *
                                 static_cast<std::size_t>(mesh.ny);
    double minimum_temperature = std::numeric_limits<double>::infinity();
    double maximum_temperature = -std::numeric_limits<double>::infinity();
    for (std::size_t xy = 0; xy < xy_count; ++xy) {
        if (result.geometry.cell_area_xy_m2[xy] <= 0.0) continue;
        for (int z = 0; z < mesh.nz; ++z) {
            const double temperature = result.temperature_k.values()[xy + xy_count *
                                                                       static_cast<std::size_t>(z)];
            minimum_temperature = std::min(minimum_temperature, temperature);
            maximum_temperature = std::max(maximum_temperature, temperature);
        }
    }
    REQUIRE(maximum_temperature - minimum_temperature > 1.0e-6);

    std::vector<double> aggregate_area(xy_count, 0.0);
    for (std::size_t xy = 0; xy < xy_count; ++xy) {
        const int root = result.geometry.agglomerate_parent[xy];
        if (root >= 0) aggregate_area[static_cast<std::size_t>(root)] +=
            result.geometry.cell_area_xy_m2[xy];
    }

    TabulatedWaterProperties water;
    const double porosity =
        compress_puck(recipe, coefficients,
                      recipe.pressure_pa.max_value() - coefficients.outlet_pressure_pa)
            .porosity;
    double expected_weighted_temperature = 0.0;
    double thermal_capacity_total = 0.0;
    for (std::size_t xy = 0; xy < xy_count; ++xy) {
        if (result.geometry.agglomerate_parent[xy] != static_cast<int>(xy)) continue;
        const double area = aggregate_area[xy];
        for (int z = 0; z < mesh.nz; ++z) {
            const std::size_t index = xy + xy_count * static_cast<std::size_t>(z);
            const double temperature = result.temperature_k.values()[index];
            const double capacity = area * result.geometry.dz_m * porosity *
                                    water.density_kg_m3(temperature);
            const double retained = result.saturation.values()[index] * capacity;
            const double dose_share = recipe.dose_kg * area / recipe.basket_area_m2() /
                                      static_cast<double>(mesh.nz);
            const double thermal_capacity =
                dose_share * coefficients.coffee_heat_capacity_j_kg_k +
                retained * water.heat_capacity_j_kg_k(temperature);
            thermal_capacity_total += thermal_capacity;
            expected_weighted_temperature += thermal_capacity * temperature;
        }
    }

    REQUIRE(thermal_capacity_total > 0.0);
    REQUIRE(result.samples.back().puck_temperature_k ==
            Catch::Approx(expected_weighted_temperature / thermal_capacity_total).margin(1.0e-9));
}

// Regression: cfd3d_artifact_io::result_hash() used to hash dump_case_json()
// verbatim, which embeds coefficient provenance -- unlike the Level 1-3
// pipeline's artifact_io::coefficient_hash(), which explicitly erases
// provenance for exactly this reason (hashing.cpp). Attaching or editing a
// provenance note is not a physics change, so it must not change
// result_hash/run_id.
TEST_CASE("cfd3d result_hash is unaffected by coefficient provenance", "[cfd3d][artifacts]") {
    const cfd3d_artifact_io::Cfd3dCase with_provenance{short_recipe(),
                                                       testing::baseline_coefficients(),
                                                       small_config()};
    REQUIRE(with_provenance.coefficients.provenance.has_value());

    cfd3d_artifact_io::Cfd3dCase without_provenance = with_provenance;
    without_provenance.coefficients.provenance.reset();

    const Cfd3dResult first =
        Cfd3dSolver().run(with_provenance.recipe, with_provenance.coefficients,
                          with_provenance.config);
    const Cfd3dResult second =
        Cfd3dSolver().run(without_provenance.recipe, without_provenance.coefficients,
                          without_provenance.config);

    const std::string hash_with =
        cfd3d_artifact_io::result_hash(with_provenance, first, {});
    const std::string hash_without =
        cfd3d_artifact_io::result_hash(without_provenance, second, {});
    REQUIRE(hash_with == hash_without);
}

TEST_CASE("cfd3d emits ordered initial and final snapshots", "[cfd3d][artifacts]") {
    Recipe recipe = short_recipe();
    Cfd3dConfig config = small_config();
    std::vector<Cfd3dSnapshot> snapshots;
    config.snapshot_sink = [&](const Cfd3dSnapshot& snapshot) { snapshots.push_back(snapshot); };

    const Cfd3dResult result = Cfd3dSolver().run(recipe, testing::baseline_coefficients(), config);

    REQUIRE(result.elapsed_time_s > 0.0);
    REQUIRE(snapshots.size() >= 2);
    REQUIRE(snapshots.front().time_s == 0.0);
    REQUIRE(snapshots.back().time_s == Catch::Approx(result.elapsed_time_s));
    for (std::size_t index = 1; index < snapshots.size(); ++index) {
        REQUIRE(snapshots[index].time_s > snapshots[index - 1].time_s);
        REQUIRE(snapshots[index].pressure_pa.size() == 6U * 6U * 8U);
    }
}

TEST_CASE("cfd3d material fields create three-dimensional flow structure",
          "[cfd3d][integration]") {
    Recipe recipe = short_recipe();
    Cfd3dConfig config = small_config();
    config.snapshot_interval_s = 0.0;
    config.material = Cfd3dMaterialField(config.mesh.nx, config.mesh.ny, config.mesh.nz, 1.0);
    for (int z = 0; z < config.mesh.nz; ++z) {
        for (int y = 0; y < config.mesh.ny; ++y) {
            config.material.at(config.mesh.nx - 1, y, z) = 8.0;
        }
    }

    const Cfd3dResult result =
        Cfd3dSolver().run(recipe, testing::baseline_coefficients(), config);
    double maximum_lateral_velocity = 0.0;
    for (const double value : result.velocity_x_m_s.values()) {
        maximum_lateral_velocity = std::max(maximum_lateral_velocity, std::abs(value));
    }
    for (const double value : result.velocity_y_m_s.values()) {
        maximum_lateral_velocity = std::max(maximum_lateral_velocity, std::abs(value));
    }
    REQUIRE(result.termination == TerminationReason::target_mass_reached);
    REQUIRE(maximum_lateral_velocity > 0.0);
}

TEST_CASE("cfd3d rejects unsupported mesh and snapshot budgets", "[cfd3d][artifacts]") {
    Recipe recipe = testing::baseline_recipe();
    ModelCoefficients coefficients = testing::baseline_coefficients();

    Cfd3dConfig product_limit;
    product_limit.mesh = {128, 128, 17};
    REQUIRE_THROWS_AS(Cfd3dSolver().run(recipe, coefficients, product_limit), InvalidInputError);

    Cfd3dConfig snapshots = small_config();
    snapshots.snapshot_interval_s = 0.01;
    REQUIRE_THROWS_AS(Cfd3dSolver().run(recipe, coefficients, snapshots), InvalidInputError);
}

// Audit F4, issue #6: root.value("schema_version", ...) only tolerates a
// *missing* key -- a present key of the wrong type still threw an uncaught
// nlohmann::json::type_error instead of a structured MALFORMED_JSON.
TEST_CASE("a wrongly typed cfd3d schema_version is a structured error", "[cfd3d][artifacts]") {
    REQUIRE_THROWS_MATCHES(
        cfd3d_artifact_io::load_case_json(R"({"schema_version":123,"recipe":{}})"), artifact_io::LoadError,
        Catch::Matchers::Predicate<artifact_io::LoadError>([](const artifact_io::LoadError& e) {
            return e.code == "MALFORMED_JSON" && e.path == "cfd3d.schema_version";
        }));
}

// Audit P3, issue #20: Cfd3dField/Cfd3dMaterialField's 4-arg constructors
// are public and called field_size() directly, which had no checked
// multiplication and no maximum -- a caller constructing one directly
// (bypassing both Cfd3dSolver::run()'s mesh check and
// cfd3d_artifact_io::validate_mesh_bounds, issue #5's loader-side guard for
// the JSON path) could overflow std::size_t or force an unbounded
// allocation. Both constructors must now enforce the same 128x128x256 /
// 262144-cell policy the solver and loader already do.
TEST_CASE("Cfd3dField/Cfd3dMaterialField constructors reject oversized and overflowing dimensions",
          "[cfd3d]") {
    SECTION("negative dimensions are still rejected") {
        REQUIRE_THROWS_AS(Cfd3dField(-1, 4, 4), std::invalid_argument);
        REQUIRE_THROWS_AS(Cfd3dMaterialField(4, -1, 4), std::invalid_argument);
    }
    SECTION("zero dimensions are rejected") { REQUIRE_THROWS_AS(Cfd3dField(0, 4, 4), std::invalid_argument); }
    SECTION("one axis over the per-axis cap is rejected") {
        REQUIRE_THROWS_AS(Cfd3dField(129, 1, 1), std::invalid_argument);
        REQUIRE_THROWS_AS(Cfd3dMaterialField(1, 1, 257), std::invalid_argument);
    }
    SECTION("within each axis cap but over the cell-product cap is rejected") {
        REQUIRE_THROWS_AS(Cfd3dField(100, 100, 100), std::invalid_argument);
    }
    SECTION("dimensions large enough to overflow size_t are rejected, not truncated") {
        constexpr int huge = 1'000'000'000;
        REQUIRE_THROWS_AS(Cfd3dField(huge, huge, huge), std::invalid_argument);
    }
    SECTION("a mesh within every limit still constructs normally") {
        const Cfd3dField field(6, 6, 8, 2.5);
        REQUIRE(field.size() == 6u * 6u * 8u);
        REQUIRE(field.at(0, 0, 0) == Catch::Approx(2.5));
    }
}

// Audit F2, issue #5: parse_material() built a dense Cfd3dMaterialField from
// mesh.nx/ny/nz before the solver's mesh-limit validation ran, so an
// oversized scalar/uniform material request forced a large allocation on
// load. The loader must reject the mesh before allocating anything.
TEST_CASE("load_case_json rejects an oversized scalar material without allocating a field",
          "[cfd3d][artifacts]") {
    using nlohmann::json;
    json root = json::parse(cfd3d_artifact_io::dump_case_json(
        cfd3d_artifact_io::Cfd3dCase{testing::baseline_recipe(), testing::baseline_coefficients(), {}}, -1));
    root["mesh"] = {{"nx", 129}, {"ny", 1}, {"nz", 1}};  // one past the documented 128 cap
    root["material"] = 1.0;                              // scalar/uniform: the allocating branch
    REQUIRE_THROWS_AS(cfd3d_artifact_io::load_case_json(root.dump()), artifact_io::LoadError);

    json product_root = json::parse(cfd3d_artifact_io::dump_case_json(
        cfd3d_artifact_io::Cfd3dCase{testing::baseline_recipe(), testing::baseline_coefficients(), {}}, -1));
    product_root["mesh"] = {{"nx", 100}, {"ny", 100}, {"nz", 100}};  // within each axis cap, over the 262144 product
    product_root["material"] = 1.0;
    REQUIRE_THROWS_AS(cfd3d_artifact_io::load_case_json(product_root.dump()), artifact_io::LoadError);
}

TEST_CASE("cfd3d case artifacts round trip their executable contract", "[cfd3d][artifacts]") {
    cfd3d_artifact_io::Cfd3dCase source;
    source.recipe = short_recipe();
    source.coefficients = testing::baseline_coefficients();
    source.config = small_config();
    source.config.snapshot_sink = {};
    source.config.material = Cfd3dMaterialField(6, 6, 8, 1.25);

    const cfd3d_artifact_io::Cfd3dCase loaded =
        cfd3d_artifact_io::load_case_json(cfd3d_artifact_io::dump_case_json(source, -1));
    REQUIRE(loaded.config.mesh.nx == source.config.mesh.nx);
    REQUIRE(loaded.config.mesh.ny == source.config.mesh.ny);
    REQUIRE(loaded.config.mesh.nz == source.config.mesh.nz);
    REQUIRE(loaded.config.snapshot_interval_s == source.config.snapshot_interval_s);
    REQUIRE(loaded.config.material.values() == source.config.material.values());
}
