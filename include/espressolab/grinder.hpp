#pragma once
#include <string>
#include <vector>

#include "espressolab/grind.hpp"
#include "espressolab/validation.hpp"

namespace espressolab {

// A comminution model: burr geometry in, particle size distribution out.
//
// Deliberately separate from the shot pipeline, in the same position as the CFD
// solvers (3.1). It reads no recipe and no ModelCoefficients, writes no shot
// artifact, and cannot influence a shot's result hash. What it produces is a
// GrindDistribution, which a recipe may then carry.
//
// What this solves, stated exactly so nothing is overclaimed. A standard
// population balance over a fixed logarithmic size grid: on each pass, a mass
// fraction S(d) of every size class breaks, and its mass is redistributed over
// the smaller classes by a breakage function B. Both closures are textbook
// comminution, not inventions of this project:
//
//   selection    S(d)      = S_rate * (d/d_gap - 1)^alpha, 0 at or below the gap
//   breakage     B(d_i|d_j) = (d_i / d_j)^beta          (Broadbent-Callcott)
//   fines        a fixed fraction phi of every broken parent's mass is routed
//                to the cell-wall mode instead of the coarse progeny
//
// The fines term is what produces the bimodality real coffee grinds show. It is
// physically motivated -- roasted bean cell walls fracture at their own
// characteristic scale rather than scaling with the parent fragment -- and it
// is two interpretable numbers rather than a fitted curve.
//
// It is NOT validated. There is no measured PSD in this repository to check it
// against, and its coefficients are a plausible baseline in exactly the sense
// the shot model's default coefficients are (see docs/model.md). It is also not
// a grinder dial model: the burr gap is a physical length in microns, and
// nothing here maps a dial number onto one.
struct GrinderSpec {
    std::string name = "unnamed";

    // Burr gap, the length scale the grind is built around.
    double burr_gap_um = 400.0;
    // Whole-bean feed size. Breakage starts here.
    double bean_diameter_um = 6000.0;

    // How many times the population is passed through the breakage operator.
    // Stands in for residence time in the burrs.
    int passes = 12;

    // Selection: how readily an oversize particle breaks, and how steeply that
    // rises with size. A particle at or below the gap has passed through the
    // burrs and is finished product -- it is never selected again, which is the
    // classification a real burr set performs. `selection_rate` is therefore
    // the breakage propensity at twice the gap, not at the gap itself. A
    // darker, more brittle roast breaks more readily: a higher rate.
    double selection_rate = 0.35;
    double selection_exponent = 1.2;

    // Breakage: the Broadbent-Callcott exponent. Larger values push progeny
    // toward the top of the available range, i.e. a narrower grind.
    double breakage_exponent = 1.4;

    // Cell-wall fines: the mass fraction of each breakage *event* routed to the
    // fines mode, and the size that mode sits at. Note this is per event, not
    // per unit feed: mass is broken repeatedly on its way down from whole
    // beans, so the cumulative fines fraction lands several times higher than
    // this number (roughly 3-4x at the default settings). 0.01 puts the total
    // in the low single-digit percent by mass, which is the order real grinds
    // show -- fines dominate by particle count, not by mass.
    double fines_yield = 0.01;
    double fines_diameter_um = 40.0;

    // Output grid. Bins are logarithmically spaced from fines_diameter_um to
    // bean_diameter_um; the returned distribution keeps those carrying mass.
    int bins = 24;

    [[nodiscard]] ValidationResult validate() const;
};

struct GrinderResult {
    GrindDistribution distribution;
    // Reported for convenience; both are properties of `distribution`.
    double sauter_mean_diameter_um = 0.0;
    double geometric_std_dev = 0.0;
    // Mass that left the coarse population into the cell-wall mode, summed
    // over every pass. Diagnostic only.
    double cumulative_fines_fraction = 0.0;
    // Largest deviation from unit mass observed across the passes. The
    // population balance conserves mass exactly, so this exists to prove it.
    double mass_balance_residual = 0.0;
};

// Deterministic: no RNG anywhere, and a fixed evaluation order, so the same
// spec always yields the same distribution.
[[nodiscard]] GrinderResult grind(const GrinderSpec& spec);

}  // namespace espressolab
