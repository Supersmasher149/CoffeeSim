#pragma once

namespace espressolab {

// Section 7.1: viscosity, density and heat capacity sit behind an interface so
// the starting interpolation table can be replaced by a better fit later
// without touching the solver.
class WaterProperties {
public:
    virtual ~WaterProperties() = default;
    [[nodiscard]] virtual double density_kg_m3(double temperature_k) const = 0;
    [[nodiscard]] virtual double viscosity_pa_s(double temperature_k) const = 0;
    [[nodiscard]] virtual double heat_capacity_j_kg_k(double temperature_k) const = 0;
    [[nodiscard]] virtual double min_temperature_k() const = 0;
    [[nodiscard]] virtual double max_temperature_k() const = 0;
};

// Small validated table over 273.15-373.15 K with linear interpolation. Chosen
// over an embedded correlation because it is easier to test and to explain
// (7.1, "Implementation choice").
class TabulatedWaterProperties final : public WaterProperties {
public:
    [[nodiscard]] double density_kg_m3(double temperature_k) const override;
    [[nodiscard]] double viscosity_pa_s(double temperature_k) const override;
    [[nodiscard]] double heat_capacity_j_kg_k(double temperature_k) const override;
    [[nodiscard]] double min_temperature_k() const override;
    [[nodiscard]] double max_temperature_k() const override;
};

}  // namespace espressolab
