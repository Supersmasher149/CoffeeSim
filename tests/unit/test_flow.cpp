#include <catch_amalgamated.hpp>

#include "espressolab/puck.hpp"
#include "espressolab/units.hpp"

using namespace espressolab;

namespace {
constexpr double kArea = 2.6421e-3;      // 58 mm basket
constexpr double kViscosity = 3.05e-4;   // water near 93 C
constexpr double kDepth = 0.009;
constexpr double kPermeability = 1.4e-15;
constexpr double kMaxFlow = 5.0e-5;
}  // namespace

// Section 14.1: zero pressure gives zero flow; higher pressure raises flow when
// the rest of the state is fixed.
TEST_CASE("zero and negative pressure difference give zero flow", "[flow]") {
    const FlowSolution none =
        darcy_flow(kPermeability, kArea, kViscosity, kDepth, 0.0, kMaxFlow);
    REQUIRE(none.flow_m3_s == 0.0);

    const FlowSolution reversed =
        darcy_flow(kPermeability, kArea, kViscosity, kDepth, -1.0e5, kMaxFlow);
    REQUIRE(reversed.flow_m3_s == 0.0);
    REQUIRE(reversed.clamped_by_backpressure);
}

TEST_CASE("flow is proportional to pressure difference", "[flow]") {
    const FlowSolution low =
        darcy_flow(kPermeability, kArea, kViscosity, kDepth, units::bar_to_pa(3.0), kMaxFlow);
    const FlowSolution high =
        darcy_flow(kPermeability, kArea, kViscosity, kDepth, units::bar_to_pa(9.0), kMaxFlow);

    REQUIRE(high.flow_m3_s > low.flow_m3_s);
    REQUIRE(high.flow_m3_s / low.flow_m3_s == Catch::Approx(3.0));
}

TEST_CASE("greater permeability never reduces flow", "[flow]") {
    double previous = 0.0;
    for (double k = 1.0e-16; k < 1.0e-14; k *= 1.2) {
        const FlowSolution flow =
            darcy_flow(k, kArea, kViscosity, kDepth, units::bar_to_pa(9.0), kMaxFlow);
        REQUIRE(flow.flow_m3_s >= previous);
        previous = flow.flow_m3_s;
    }
}

TEST_CASE("nonpositive divisors return zero flow instead of dividing", "[flow]") {
    const double dp = units::bar_to_pa(9.0);
    REQUIRE(darcy_flow(0.0, kArea, kViscosity, kDepth, dp, kMaxFlow).flow_m3_s == 0.0);
    REQUIRE(darcy_flow(kPermeability, 0.0, kViscosity, kDepth, dp, kMaxFlow).flow_m3_s == 0.0);
    REQUIRE(darcy_flow(kPermeability, kArea, 0.0, kDepth, dp, kMaxFlow).flow_m3_s == 0.0);
    REQUIRE(darcy_flow(kPermeability, kArea, kViscosity, 0.0, dp, kMaxFlow).flow_m3_s == 0.0);
}

TEST_CASE("the maximum flow guard reports that it fired", "[flow]") {
    // A permeability far outside the physical range must be caught by the guard
    // and flagged, never silently returned (6.5).
    const FlowSolution flow =
        darcy_flow(1.0e-9, kArea, kViscosity, kDepth, units::bar_to_pa(9.0), kMaxFlow);
    REQUIRE(flow.clamped_by_max_flow);
    REQUIRE(flow.flow_m3_s == Catch::Approx(kMaxFlow));
    REQUIRE(flow.unclamped_flow_m3_s > flow.flow_m3_s);
}
