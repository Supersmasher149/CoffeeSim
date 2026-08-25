#pragma once
#include <memory>
#include <stdexcept>

#include "espressolab/result.hpp"
#include "espressolab/types.hpp"
#include "espressolab/water_properties.hpp"

namespace espressolab {

// Section 9.1: fixed 0.01 s internal step, sampled every 0.05 s.
struct SimulationConfig {
    double dt_s = 0.01;
    double sample_interval_s = 0.05;
    bool strict_invariants = true;
};

// Thrown for input that fails validation before any stepping happens. A run
// that starts always finishes with a recorded termination reason instead.
class InvalidInputError : public std::runtime_error {
public:
    explicit InvalidInputError(const ValidationResult& result);
    [[nodiscard]] const ValidationResult& validation() const { return validation_; }

private:
    ValidationResult validation_;
};

class Simulator {
public:
    Simulator();
    explicit Simulator(std::shared_ptr<const WaterProperties> water);

    [[nodiscard]] ShotResult run(const Recipe& recipe, const ModelCoefficients& coeff,
                                 const SimulationConfig& config = {}) const;

private:
    std::shared_ptr<const WaterProperties> water_;
};

}  // namespace espressolab
