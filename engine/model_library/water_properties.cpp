#include "espressolab/water_properties.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

#include "espressolab/units.hpp"

namespace espressolab {
namespace {

struct Knot {
    double temperature_c;
    double density_kg_m3;
    double viscosity_mpa_s;
    double heat_capacity_j_kg_k;
};

// Saturated liquid water at atmospheric pressure. Small enough to eyeball in a
// review, and every knot is asserted in tests/unit/test_water_properties.cpp.
constexpr std::array<Knot, 11> kTable{{
    {0.0, 999.84, 1.7914, 4219.0},
    {10.0, 999.70, 1.3060, 4195.0},
    {20.0, 998.21, 1.0016, 4184.0},
    {30.0, 995.65, 0.7972, 4180.0},
    {40.0, 992.22, 0.6527, 4179.0},
    {50.0, 988.04, 0.5465, 4181.0},
    {60.0, 983.20, 0.4660, 4185.0},
    {70.0, 977.76, 0.4035, 4190.0},
    {80.0, 971.79, 0.3540, 4196.0},
    {90.0, 965.31, 0.3142, 4205.0},
    {100.0, 958.35, 0.2816, 4216.0},
}};

// Clamped linear interpolation: the solver is responsible for warning when it
// asks for a temperature outside the table (7.3), the table itself simply
// holds its end values.
double interpolate(double temperature_k, double Knot::*field) {
    const double t_c = units::kelvin_to_celsius(temperature_k);
    if (t_c <= kTable.front().temperature_c) return kTable.front().*field;
    if (t_c >= kTable.back().temperature_c) return kTable.back().*field;

    for (std::size_t i = 1; i < kTable.size(); ++i) {
        if (t_c <= kTable[i].temperature_c) {
            const Knot& lo = kTable[i - 1];
            const Knot& hi = kTable[i];
            const double span = hi.temperature_c - lo.temperature_c;
            const double f = (t_c - lo.temperature_c) / span;
            return lo.*field + f * (hi.*field - lo.*field);
        }
    }
    return kTable.back().*field;
}

}  // namespace

double TabulatedWaterProperties::density_kg_m3(double temperature_k) const {
    return interpolate(temperature_k, &Knot::density_kg_m3);
}

double TabulatedWaterProperties::viscosity_pa_s(double temperature_k) const {
    return interpolate(temperature_k, &Knot::viscosity_mpa_s) * 1.0e-3;
}

double TabulatedWaterProperties::heat_capacity_j_kg_k(double temperature_k) const {
    return interpolate(temperature_k, &Knot::heat_capacity_j_kg_k);
}

double TabulatedWaterProperties::min_temperature_k() const {
    return units::celsius_to_kelvin(kTable.front().temperature_c);
}

double TabulatedWaterProperties::max_temperature_k() const {
    return units::celsius_to_kelvin(kTable.back().temperature_c);
}

}  // namespace espressolab
