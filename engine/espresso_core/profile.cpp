#include "espressolab/profile.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace espressolab {

PiecewiseLinearProfile::PiecewiseLinearProfile(std::vector<ProfilePoint> points)
    : points_(std::move(points)) {
    slopes_.assign(points_.empty() ? 0 : points_.size() - 1, 0.0);
    for (std::size_t i = 1; i < points_.size(); ++i) {
        const double dt = points_[i].time_s - points_[i - 1].time_s;
        slopes_[i - 1] = dt > 0.0 ? (points_[i].value - points_[i - 1].value) / dt : 0.0;
    }
}

PiecewiseLinearProfile PiecewiseLinearProfile::constant(double value) {
    return PiecewiseLinearProfile({{0.0, value}});
}

double PiecewiseLinearProfile::sample(double time_s) const {
    if (points_.empty()) return 0.0;
    // Hold the first value before the first point and the final value after the
    // last point (5.2).
    if (time_s <= points_.front().time_s) return points_.front().value;
    if (time_s >= points_.back().time_s) return points_.back().value;

    for (std::size_t i = 1; i < points_.size(); ++i) {
        if (time_s <= points_[i].time_s) {
            const double dt = time_s - points_[i - 1].time_s;
            return points_[i - 1].value + slopes_[i - 1] * dt;
        }
    }
    return points_.back().value;
}

ValidationResult PiecewiseLinearProfile::validate(const char* path) const {
    ValidationResult result;
    if (points_.empty()) {
        result.add("EMPTY_PROFILE", std::string(path) + " requires at least one point", path);
        return result;
    }
    for (std::size_t i = 0; i < points_.size(); ++i) {
        const std::string point_path = std::string(path) + "[" + std::to_string(i) + "]";
        if (!std::isfinite(points_[i].time_s) || !std::isfinite(points_[i].value)) {
            result.add("NONFINITE_INPUT", point_path + " must contain finite numbers", point_path);
            continue;
        }
        if (points_[i].time_s < 0.0) {
            result.add("NONPHYSICAL_INPUT", point_path + " time must not be negative", point_path);
        }
        // Strictly increasing time values (5.2).
        if (i > 0 && points_[i].time_s <= points_[i - 1].time_s) {
            result.add("UNORDERED_PROFILE",
                       point_path + " time must be strictly greater than the previous point",
                       point_path);
        }
    }
    return result;
}

double PiecewiseLinearProfile::last_time_s() const {
    return points_.empty() ? 0.0 : points_.back().time_s;
}

double PiecewiseLinearProfile::min_value() const {
    if (points_.empty()) return 0.0;
    return std::min_element(points_.begin(), points_.end(),
                            [](const ProfilePoint& a, const ProfilePoint& b) {
                                return a.value < b.value;
                            })->value;
}

double PiecewiseLinearProfile::max_value() const {
    if (points_.empty()) return 0.0;
    return std::max_element(points_.begin(), points_.end(),
                            [](const ProfilePoint& a, const ProfilePoint& b) {
                                return a.value < b.value;
                            })->value;
}

}  // namespace espressolab
