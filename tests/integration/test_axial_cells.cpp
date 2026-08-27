#include <catch_amalgamated.hpp>
#include <cmath>

#include <nlohmann/json.hpp>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/artifact_io.hpp"
#include "espressolab/simulator.hpp"

using namespace espressolab;

namespace {

Recipe with_cells(int cells) {
    Recipe recipe = testing::baseline_recipe();
    recipe.axial_cells = cells;
    return recipe;
}

ShotResult run_with_cells(int cells) {
    return Simulator().run(with_cells(cells), testing::baseline_coefficients());
}

}  // namespace

// Section 4.1, fidelity level 3. One cell is the Level 2 lumped puck, so the
// discretization has to leave it exactly where it was.
TEST_CASE("a single axial cell reproduces the Level 2 shot", "[axial][integration]") {
    const ShotResult lumped = Simulator().run(testing::baseline_recipe(),
                                              testing::baseline_coefficients());
    const ShotResult single = run_with_cells(1);

    REQUIRE(single.summary.elapsed_time_s == Catch::Approx(lumped.summary.elapsed_time_s));
    REQUIRE(single.summary.beverage_mass_kg == Catch::Approx(lumped.summary.beverage_mass_kg));
    REQUIRE(single.summary.tds_fraction == Catch::Approx(lumped.summary.tds_fraction));
    REQUIRE(single.summary.extraction_yield_fraction ==
            Catch::Approx(lumped.summary.extraction_yield_fraction));
    REQUIRE(single.regions.front().cells.size() == 1);
}

TEST_CASE("axial refinement converges as cells double", "[axial][convergence]") {
    // Each doubling must move the answer by less than the doubling before it.
    // A discretization that does not settle is not resolving anything.
    const ShotResult c2 = run_with_cells(2);
    const ShotResult c4 = run_with_cells(4);
    const ShotResult c8 = run_with_cells(8);
    const ShotResult c16 = run_with_cells(16);

    const auto time_gap = [](const ShotResult& a, const ShotResult& b) {
        return std::abs(a.summary.elapsed_time_s - b.summary.elapsed_time_s);
    };
    const auto yield_gap = [](const ShotResult& a, const ShotResult& b) {
        return std::abs(a.summary.extraction_yield_fraction - b.summary.extraction_yield_fraction);
    };

    REQUIRE(time_gap(c4, c2) > time_gap(c8, c4));
    REQUIRE(time_gap(c8, c4) > time_gap(c16, c8));
    REQUIRE(yield_gap(c4, c2) > yield_gap(c8, c4));
    REQUIRE(yield_gap(c8, c4) > yield_gap(c16, c8));
}

TEST_CASE("the wetting front advances down the column", "[axial][integration]") {
    // Stopped mid pre-infusion: the front is partway down, so an upper cell is
    // never drier than the cell beneath it, and somewhere there is a real edge.
    Recipe recipe = with_cells(8);
    recipe.target_beverage_mass_kg.reset();
    recipe.maximum_time_s = 10.0;

    const ShotResult result = Simulator().run(recipe, testing::baseline_coefficients());
    const std::vector<AxialCellSummary>& cells = result.regions.front().cells;
    REQUIRE(cells.size() == 8);

    bool monotone = true;
    bool has_edge = false;
    for (std::size_t i = 0; i + 1 < cells.size(); ++i) {
        if (cells[i].saturation < cells[i + 1].saturation - 1.0e-12) monotone = false;
        if (cells[i].saturation > cells[i + 1].saturation + 1.0e-9) has_edge = true;
    }
    INFO("top " << cells.front().saturation << " bottom " << cells.back().saturation);
    REQUIRE(monotone);
    REQUIRE(has_edge);

    // The front has not reached the basket, so nothing has left the puck.
    REQUIRE(cells.back().saturation == Catch::Approx(0.0).margin(1.0e-12));
    REQUIRE(result.summary.beverage_mass_kg == Catch::Approx(0.0).margin(1.0e-12));
}

TEST_CASE("solute concentration rises down the column", "[axial][integration]") {
    // The mechanism the lumped puck could not express: liquid arrives at each
    // cell already carrying what the cells above it gave up, so the driving
    // gradient falls with depth and so does the local yield.
    const ShotResult result = run_with_cells(8);
    const std::vector<AxialCellSummary>& cells = result.regions.front().cells;

    for (std::size_t i = 0; i + 1 < cells.size(); ++i) {
        INFO("cell " << i << " -> " << i + 1);
        REQUIRE(cells[i].pore_tds_fraction < cells[i + 1].pore_tds_fraction);
        REQUIRE(cells[i].extraction_yield_fraction > cells[i + 1].extraction_yield_fraction);
    }

    // Water enters hot and loses heat to the bed on the way down.
    REQUIRE(cells.front().temperature_k > cells.back().temperature_k);
}

TEST_CASE("mass balances close across the axial grid", "[axial][invariants]") {
    for (const int cells : {1, 3, 8, 32}) {
        INFO("axial cells: " << cells);
        const ShotResult result = run_with_cells(cells);
        REQUIRE(std::abs(result.diagnostics.water_mass_residual_kg) < 1.0e-9);
        REQUIRE(std::abs(result.diagnostics.solids_mass_residual_kg) < 1.0e-9);
        REQUIRE(result.regions.front().cells.size() == static_cast<std::size_t>(cells));

        // Every cell holds a legal saturation and gave up no more than it had.
        for (const AxialCellSummary& cell : result.regions.front().cells) {
            REQUIRE(cell.saturation >= 0.0);
            REQUIRE(cell.saturation <= 1.0 + 1.0e-9);
            REQUIRE(cell.extraction_yield_fraction >= 0.0);
            REQUIRE(std::isfinite(cell.pore_tds_fraction));
        }
    }
}

TEST_CASE("an axial run reproduces its own result hash", "[axial][artifacts]") {
    const Recipe recipe = with_cells(6);
    const ModelCoefficients coeff = testing::baseline_coefficients();
    const SimulationConfig config;

    ShotResult first = Simulator().run(recipe, coeff, config);
    ShotResult second = Simulator().run(recipe, coeff, config);
    artifact_io::stamp_manifest(first, recipe, coeff, config);
    artifact_io::stamp_manifest(second, recipe, coeff, config);
    REQUIRE(first.manifest.result_hash == second.manifest.result_hash);

    // A different column is a different run, so the hash has to move.
    ShotResult other = Simulator().run(with_cells(7), coeff, config);
    artifact_io::stamp_manifest(other, with_cells(7), coeff, config);
    REQUIRE(other.manifest.result_hash != first.manifest.result_hash);
}

TEST_CASE("axial cells reach the serialized artifacts in order", "[axial][artifacts]") {
    const ShotResult result = run_with_cells(5);

    for (const std::string& document :
         {artifact_io::dump_summary_json(result), artifact_io::dump_result_json(result)}) {
        const nlohmann::json root = nlohmann::json::parse(document);
        const nlohmann::json& cells = root.at("regions").at(0).at("cells");
        REQUIRE(cells.is_array());
        REQUIRE(cells.size() == 5);

        double previous_tds = -1.0;
        for (const nlohmann::json& cell : cells) {
            for (const char* field :
                 {"saturation", "temperature_c", "pore_tds_percent", "extraction_yield_percent"}) {
                REQUIRE(cell.contains(field));
                REQUIRE(std::isfinite(cell.at(field).get<double>()));
            }
            // Serialized screen-side first, which is the order the gradient runs.
            const double tds = cell.at("pore_tds_percent").get<double>();
            REQUIRE(tds > previous_tds);
            previous_tds = tds;
        }
    }
}
