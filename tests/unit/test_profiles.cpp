#include <catch_amalgamated.hpp>
#include <vector>

#include "espressolab/profile.hpp"

using namespace espressolab;

// Section 14.1: interpolation, boundaries, single point, invalid ordering.
TEST_CASE("a profile interpolates linearly between its points", "[profile]") {
    const PiecewiseLinearProfile profile({{0.0, 2.0}, {6.0, 2.0}, {10.0, 9.0}, {30.0, 9.0}});

    REQUIRE(profile.sample(0.0) == Catch::Approx(2.0));
    REQUIRE(profile.sample(3.0) == Catch::Approx(2.0));
    REQUIRE(profile.sample(8.0) == Catch::Approx(5.5));   // halfway up the ramp
    REQUIRE(profile.sample(10.0) == Catch::Approx(9.0));
    REQUIRE(profile.sample(20.0) == Catch::Approx(9.0));
}

TEST_CASE("a profile holds its end values outside its range", "[profile]") {
    const PiecewiseLinearProfile profile({{2.0, 4.0}, {8.0, 10.0}});
    REQUIRE(profile.sample(-5.0) == Catch::Approx(4.0));
    REQUIRE(profile.sample(0.0) == Catch::Approx(4.0));
    REQUIRE(profile.sample(100.0) == Catch::Approx(10.0));
}

TEST_CASE("a single-point profile is constant everywhere", "[profile]") {
    const PiecewiseLinearProfile profile = PiecewiseLinearProfile::constant(93.0);
    REQUIRE(profile.validate("p").ok());
    REQUIRE(profile.sample(0.0) == Catch::Approx(93.0));
    REQUIRE(profile.sample(45.0) == Catch::Approx(93.0));
}

TEST_CASE("profile validation rejects empty and unordered inputs", "[profile]") {
    REQUIRE_FALSE(PiecewiseLinearProfile(std::vector<ProfilePoint>{}).validate("p").ok());

    const ValidationResult unordered =
        PiecewiseLinearProfile({{0.0, 1.0}, {5.0, 2.0}, {5.0, 3.0}}).validate("p");
    REQUIRE_FALSE(unordered.ok());
    REQUIRE(unordered.issues().front().code == "UNORDERED_PROFILE");

    const ValidationResult negative = PiecewiseLinearProfile({{-1.0, 1.0}}).validate("p");
    REQUIRE_FALSE(negative.ok());
    REQUIRE(negative.issues().front().code == "NONPHYSICAL_INPUT");
}

// sample() is a linear scan over every point, called at least once per fixed
// solver step -- an unbounded point count let a single profile multiply the
// per-step cost of every shot with no ceiling (a 2,000,000-point profile
// measurably slowed a 60s shot that otherwise runs in ~1ms).
TEST_CASE("profile validation rejects an excessive point count", "[profile]") {
    std::vector<ProfilePoint> points;
    points.reserve(1001);
    for (int i = 0; i < 1001; ++i) points.push_back({static_cast<double>(i), 5.0});
    const ValidationResult oversized = PiecewiseLinearProfile(points).validate("p");
    REQUIRE_FALSE(oversized.ok());
    REQUIRE(oversized.issues().front().code == "PROFILE_TOO_LARGE");

    points.pop_back();
    REQUIRE(points.size() == 1000);
    REQUIRE(PiecewiseLinearProfile(points).validate("p").ok());
}

TEST_CASE("min and max report the extreme declared values", "[profile]") {
    const PiecewiseLinearProfile profile({{0.0, 2.0}, {6.0, 9.0}, {12.0, 6.0}});
    REQUIRE(profile.min_value() == Catch::Approx(2.0));
    REQUIRE(profile.max_value() == Catch::Approx(9.0));
    REQUIRE(profile.last_time_s() == Catch::Approx(12.0));
}
