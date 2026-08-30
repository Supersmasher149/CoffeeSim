#pragma once
#include <cstddef>
#include <vector>

#include "espressolab/validation.hpp"

namespace espressolab {

// A measured or generated particle size distribution, in place of the single
// representative diameter of section 5.3.
//
// The scalar `Recipe::particle_diameter_m` cannot express a real grind: coffee
// distributions are bimodal (a coarse fragment mode plus a fines mode from
// ruptured cell walls), and the two modes drive opposite outputs -- fines
// dominate the surface area that sets extraction, the coarse mode dominates the
// hydraulic length scale that sets flow. `distribution_factor()` in the model
// library exists only to correct for that missing structure after the fact.
//
// Deliberately plain data with pure accessors: this is a property of the input,
// not a physics correlation, so it lives beside Recipe in the core types and the
// closures that consume it stay in the model library (3.1).
struct GrindBin {
    double diameter_m = 0.0;    // representative diameter of the bin
    double mass_fraction = 0.0; // fraction of total puck mass in this bin
};

// One bin is allowed and meaningful: it is exactly the scalar single-diameter
// case expressed as a distribution, which is what makes the two paths directly
// comparable in a test rather than only approximately so.
inline constexpr std::size_t kMinGrindBins = 1;
inline constexpr std::size_t kMaxGrindBins = 64;

// Individual bins range far wider than the scalar path's 150-800 um, and
// deliberately so: real coffee fines sit at 10-100 um, and refusing to
// represent them would defeat the point of carrying a distribution at all.
// What stays inside the scalar envelope is the *derived* Sauter mean diameter
// (checked in Recipe::validate()), because that is the number the permeability
// and extraction correlations were shaped around.
inline constexpr double kMinBinDiameterUm = 10.0;
inline constexpr double kMaxBinDiameterUm = 2000.0;

struct GrindDistribution {
    std::vector<GrindBin> bins;

    // Sauter mean diameter, d32 = 1 / sum(w_i / d_i) for mass fractions w_i.
    //
    // This is the correct polydisperse substitution for d_p in Kozeny-Carman,
    // not a new model: the permeability's length scale *is* the surface-area-to-
    // volume ratio of the bed, and d32 is by definition the diameter of the
    // monodisperse bed with the same ratio. A monodisperse distribution
    // therefore returns its own diameter exactly.
    [[nodiscard]] double sauter_mean_diameter_m() const;

    // Mass-weighted geometric standard deviation of ln(d). Dimensionless and
    // exactly 1.0 for a monodisperse distribution; grows as the distribution
    // broadens. Feeds the permeability spread penalty in the model library.
    [[nodiscard]] double geometric_std_dev() const;

    // The distribution's width expressed on the same 0.1-1.0 scale that
    // Recipe::particle_spread_factor and the model library's
    // distribution_factor() already use, so a PSD recipe and a scalar recipe
    // share one permeability penalty rather than two rival ones.
    //
    // A normalisation of this type's own geometry, not a coffee correlation --
    // which is why it lives here rather than in the model library, and why the
    // loader can derive the scalars without reaching into the physics layer.
    // The reference sigma is a fixed model choice and deliberately NOT a
    // coefficient: every member of ModelCoefficients is hashed into
    // coefficient_hash() and so into every result_hash, so adding one would
    // rewrite the hash of every existing run for a path those runs never take.
    [[nodiscard]] double equivalent_spread_factor() const;

    [[nodiscard]] ValidationResult validate() const;
};

}  // namespace espressolab
