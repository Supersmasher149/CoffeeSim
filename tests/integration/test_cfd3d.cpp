#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/artifact_io.hpp"
#include "espressolab/cfd3d.hpp"
#include "espressolab/cfd3d_artifact_io.hpp"

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
