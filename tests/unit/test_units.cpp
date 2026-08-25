#include <catch_amalgamated.hpp>

#include "espressolab/units.hpp"

using namespace espressolab::units;

// Section 14.1: known bar/Pa, C/K, gram/kg and micron/m conversions.
TEST_CASE("pressure conversions use the exact bar definition", "[units]") {
    STATIC_REQUIRE(bar_to_pa(9.0) == 900000.0);
    STATIC_REQUIRE(bar_to_pa(0.0) == 0.0);
    REQUIRE(pa_to_bar(bar_to_pa(6.5)) == Catch::Approx(6.5));
    REQUIRE(pa_to_bar(kAtmosphericPa) == Catch::Approx(1.01325));
}

TEST_CASE("temperature conversions round-trip through absolute zero offset", "[units]") {
    STATIC_REQUIRE(celsius_to_kelvin(0.0) == 273.15);
    REQUIRE(celsius_to_kelvin(93.0) == Catch::Approx(366.15));
    REQUIRE(kelvin_to_celsius(celsius_to_kelvin(88.4)) == Catch::Approx(88.4));
}

TEST_CASE("mass and length conversions round-trip", "[units]") {
    STATIC_REQUIRE(grams_to_kg(18.0) == 0.018);
    STATIC_REQUIRE(microns_to_m(350.0) == 350.0e-6);
    STATIC_REQUIRE(mm_to_m(58.0) == 0.058);
    REQUIRE(kg_to_grams(grams_to_kg(36.0)) == Catch::Approx(36.0));
    REQUIRE(m_to_microns(microns_to_m(420.0)) == Catch::Approx(420.0));
    REQUIRE(m_to_mm(mm_to_m(9.0)) == Catch::Approx(9.0));
    REQUIRE(m3_s_to_ml_s(2.0e-6) == Catch::Approx(2.0));
}
