#include "espressolab/result.hpp"

namespace espressolab {

const char* to_string(TerminationReason reason) {
    switch (reason) {
        case TerminationReason::target_mass_reached: return "target_mass_reached";
        case TerminationReason::time_limit_reached: return "time_limit_reached";
        case TerminationReason::numerical_failure: return "numerical_failure";
        case TerminationReason::invalid_state: return "invalid_state";
        case TerminationReason::not_terminated: return "not_terminated";
    }
    return "unknown";
}

}  // namespace espressolab
