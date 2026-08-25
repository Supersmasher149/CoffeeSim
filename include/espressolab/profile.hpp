#pragma once
#include <vector>

#include "espressolab/validation.hpp"

namespace espressolab {

struct ProfilePoint {
    double time_s = 0.0;
    double value = 0.0;
};

// Section 5.2: pump pressure and inlet temperature are ordered time/value
// points with linear interpolation between them. Holds the first value before
// the first point and the last value after the last point.
class PiecewiseLinearProfile {
public:
    PiecewiseLinearProfile() = default;
    explicit PiecewiseLinearProfile(std::vector<ProfilePoint> points);

    static PiecewiseLinearProfile constant(double value);

    [[nodiscard]] double sample(double time_s) const;
    [[nodiscard]] ValidationResult validate(const char* path) const;
    [[nodiscard]] const std::vector<ProfilePoint>& points() const { return points_; }
    [[nodiscard]] bool empty() const { return points_.empty(); }
    [[nodiscard]] double last_time_s() const;
    [[nodiscard]] double min_value() const;
    [[nodiscard]] double max_value() const;

private:
    std::vector<ProfilePoint> points_;
    // Precomputed segment slopes keep sampling cheap and deterministic (5.2).
    std::vector<double> slopes_;
};

}  // namespace espressolab
