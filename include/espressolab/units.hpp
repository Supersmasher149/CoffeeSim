#pragma once

// Section 4.2: all calculations are stored in SI units. Conversions live here
// and are applied only at the input and display boundaries.
namespace espressolab::units {

inline constexpr double kPaPerBar = 100000.0;
inline constexpr double kKelvinOffset = 273.15;
inline constexpr double kAtmosphericPa = 101325.0;
inline constexpr double kGasConstantJMolK = 8.314462618;

constexpr double bar_to_pa(double bar) { return bar * kPaPerBar; }
constexpr double pa_to_bar(double pa) { return pa / kPaPerBar; }
constexpr double celsius_to_kelvin(double c) { return c + kKelvinOffset; }
constexpr double kelvin_to_celsius(double k) { return k - kKelvinOffset; }
constexpr double grams_to_kg(double g) { return g / 1000.0; }
constexpr double kg_to_grams(double kg) { return kg * 1000.0; }
constexpr double microns_to_m(double um) { return um * 1.0e-6; }
constexpr double m_to_microns(double m) { return m * 1.0e6; }
constexpr double mm_to_m(double mm) { return mm / 1000.0; }
constexpr double m_to_mm(double m) { return m * 1000.0; }
constexpr double m3_s_to_ml_s(double m3_s) { return m3_s * 1.0e6; }

}  // namespace espressolab::units
