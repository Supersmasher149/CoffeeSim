#pragma once
#include <memory>
#include <stdexcept>

#include "espressolab/execution.hpp"
#include "espressolab/result.hpp"
#include "espressolab/types.hpp"
#include "espressolab/validation.hpp"
#include "espressolab/water_properties.hpp"

namespace espressolab {

// Section 9.1: fixed 0.01 s internal step, sampled every 0.05 s.
struct SimulationConfig {
    double dt_s = 0.01;
    double sample_interval_s = 0.05;
    bool strict_invariants = true;
};

// InvalidInputError now lives in validation.hpp (included above) so modules
// outside the shot pipeline can raise it too. A run that starts always
// finishes with a recorded termination reason instead of throwing.

class Simulator {
public:
    Simulator();
    explicit Simulator(std::shared_ptr<const WaterProperties> water);

    [[nodiscard]] ShotResult run(const Recipe& recipe, const ModelCoefficients& coeff,
                                 const SimulationConfig& config = {},
                                 const CancellationCallback& is_cancelled = {}) const;

private:
    std::shared_ptr<const WaterProperties> water_;
};

}  // namespace espressolab
