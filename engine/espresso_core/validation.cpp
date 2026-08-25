#include "espressolab/validation.hpp"

#include <cmath>
#include <sstream>

namespace espressolab {

std::string ValidationResult::summary() const {
    if (issues_.empty()) return "ok";
    std::ostringstream out;
    for (std::size_t i = 0; i < issues_.size(); ++i) {
        if (i > 0) out << "; ";
        out << issues_[i].path << ": " << issues_[i].message;
    }
    return out.str();
}

void require_in_range(ValidationResult& result, double value, double low, double high,
                      const char* path, const char* unit) {
    if (!std::isfinite(value)) {
        result.add("NONFINITE_INPUT", std::string(path) + " must be a finite number", path);
        return;
    }
    if (value < low || value > high) {
        std::ostringstream msg;
        msg << path << " must be between " << low << " and " << high << " " << unit
            << " (received " << value << ")";
        result.add("OUT_OF_RANGE", msg.str(), path);
    }
}

void require_positive(ValidationResult& result, double value, const char* path) {
    if (!std::isfinite(value)) {
        result.add("NONFINITE_INPUT", std::string(path) + " must be a finite number", path);
        return;
    }
    if (value <= 0.0) {
        result.add("NONPHYSICAL_INPUT", std::string(path) + " must be greater than zero", path);
    }
}

}  // namespace espressolab
