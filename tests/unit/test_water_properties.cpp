#include <catch_amalgamated.hpp>

#include "espressolab/units.hpp"
#include "espressolab/water_properties.hpp"

using namespace espressolab;

// Section 14.1: table knots, interpolation, range handling, positive viscosity.
TEST_CASE("water property table reproduces its knots", "[water]") {
    const TabulatedWaterProperties water;

    REQUIRE(water.density_kg_m3(units::celsius_to_kelvin(20.0)) == Catch::Approx(998.21));
    REQUIRE(water.viscosity_pa_s(units::celsius_to_kelvin(20.0)) == Catch::Approx(1.0016e-3));
    REQUIRE(water.heat_capacity_j_kg_k(units::celsius_to_kelvin(20.0)) == Catch::Approx(4184.0));
    REQUIRE(water.viscosity_pa_s(units::celsius_to_kelvin(90.0)) == Catch::Approx(0.3142e-3));
}

TEST_CASE("water property table interpolates between knots", "[water]") {
    const TabulatedWaterProperties water;
    const double at_95 = water.viscosity_pa_s(units::celsius_to_kelvin(95.0));
    REQUIRE(at_95 == Catch::Approx((0.3142e-3 + 0.2816e-3) / 2.0));
}

TEST_CASE("viscosity falls as temperature rises and stays positive", "[water]") {
    const TabulatedWaterProperties water;
    double previous = water.viscosity_pa_s(units::celsius_to_kelvin(0.0));
    for (double c = 5.0; c <= 100.0; c += 5.0) {
        const double current = water.viscosity_pa_s(units::celsius_to_kelvin(c));
        REQUIRE(current > 0.0);
        REQUIRE(current < previous);
        previous = current;
    }
}

TEST_CASE("temperatures outside the table clamp to its end values", "[water]") {
    const TabulatedWaterProperties water;
    REQUIRE(water.viscosity_pa_s(200.0) == Catch::Approx(1.7914e-3));
    REQUIRE(water.viscosity_pa_s(500.0) == Catch::Approx(0.2816e-3));
    REQUIRE(water.min_temperature_k() == Catch::Approx(273.15));
    REQUIRE(water.max_temperature_k() == Catch::Approx(373.15));
}
