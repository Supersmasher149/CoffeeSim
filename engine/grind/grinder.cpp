#include "espressolab/grinder.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "espressolab/units.hpp"

namespace espressolab {
namespace {

// Mass below this in a bin is treated as absent, so a distribution does not
// carry sixty bins of numerical dust. Small enough to be far below anything a
// sieve or laser diffractometer would report.
constexpr double kMassFloor = 1.0e-6;

// Logarithmically spaced grid from the fines mode up to whole beans. Log
// spacing is what comminution work uses, and it keeps resolution where the
// distribution actually varies.
std::vector<double> build_grid(const GrinderSpec& spec) {
    const std::size_t count = static_cast<std::size_t>(spec.bins);
    std::vector<double> grid(count, 0.0);
    const double log_low = std::log(spec.fines_diameter_um);
    const double log_high = std::log(spec.bean_diameter_um);
    const double step = (log_high - log_low) / static_cast<double>(count - 1);
    for (std::size_t i = 0; i < count; ++i) {
        grid[i] = std::exp(log_low + step * static_cast<double>(i));
    }
    return grid;
}

// Austin's power-law selection function with the classification a burr set
// physically performs: a particle at or below the gap has left the grinding
// zone and is finished product, so it is not selected again. Without that
// cutoff every class keeps grinding toward the fines mode and the model
// converges on a far finer distribution than any real grinder produces.
//
//   S(d) = 0                                     for d <= gap
//   S(d) = clamp(S_rate * (d/gap - 1)^alpha, 0, 1) otherwise
double selection(double diameter_um, const GrinderSpec& spec) {
    const double gap_um = std::max(spec.burr_gap_um, 1.0e-9);
    if (diameter_um <= gap_um) return 0.0;
    const double excess = diameter_um / gap_um - 1.0;
    return std::clamp(spec.selection_rate * std::pow(excess, spec.selection_exponent), 0.0, 1.0);
}

}  // namespace

ValidationResult GrinderSpec::validate() const {
    ValidationResult result;
    require_in_range(result, burr_gap_um, 50.0, 1500.0, "grinder.burr_gap_um", "um");
    require_in_range(result, bean_diameter_um, 2000.0, 12000.0, "grinder.bean_diameter_um", "um");
    if (bean_diameter_um <= burr_gap_um) {
        result.add("NONPHYSICAL_INPUT",
                   "grinder.bean_diameter_um must exceed grinder.burr_gap_um: there is nothing to "
                   "break otherwise",
                   "grinder.bean_diameter_um");
    }
    if (passes < 1 || passes > 200) {
        result.add("NONPHYSICAL_INPUT", "grinder.passes must be between 1 and 200",
                   "grinder.passes");
    }
    require_in_range(result, selection_rate, 0.001, 1.0, "grinder.selection_rate", "");
    require_in_range(result, selection_exponent, 0.1, 5.0, "grinder.selection_exponent", "");
    require_in_range(result, breakage_exponent, 0.1, 5.0, "grinder.breakage_exponent", "");
    require_in_range(result, fines_yield, 0.0, 0.5, "grinder.fines_yield", "");
    require_in_range(result, fines_diameter_um, 5.0, 200.0, "grinder.fines_diameter_um", "um");
    if (fines_diameter_um >= burr_gap_um) {
        result.add("NONPHYSICAL_INPUT",
                   "grinder.fines_diameter_um must be smaller than grinder.burr_gap_um",
                   "grinder.fines_diameter_um");
    }
    if (bins < 4 || static_cast<std::size_t>(bins) > kMaxGrindBins) {
        result.add("NONPHYSICAL_INPUT",
                   "grinder.bins must be between 4 and " + std::to_string(kMaxGrindBins),
                   "grinder.bins");
    }
    return result;
}

GrinderResult grind(const GrinderSpec& spec) {
    const ValidationResult validation = spec.validate();
    if (!validation.ok()) throw InvalidInputError(validation);

    const std::vector<double> grid = build_grid(spec);
    const std::size_t count = grid.size();

    // Precompute the breakage matrix once: B(i|j) is the share of a broken
    // parent in class j that lands in class i. Broadbent-Callcott, normalised
    // over the classes actually below the parent so mass is conserved exactly
    // rather than to within the discretisation.
    std::vector<std::vector<double>> breakage(count, std::vector<double>(count, 0.0));
    for (std::size_t j = 1; j < count; ++j) {
        double total = 0.0;
        for (std::size_t i = 0; i < j; ++i) {
            breakage[j][i] = std::pow(grid[i] / grid[j], spec.breakage_exponent);
            total += breakage[j][i];
        }
        if (total > 0.0) {
            for (std::size_t i = 0; i < j; ++i) breakage[j][i] /= total;
        }
    }

    // The feed is whole beans: all mass in the top class.
    std::vector<double> mass(count, 0.0);
    mass.back() = 1.0;

    GrinderResult result;
    for (int pass = 0; pass < spec.passes; ++pass) {
        std::vector<double> next(count, 0.0);
        for (std::size_t j = 0; j < count; ++j) {
            if (mass[j] <= 0.0) continue;
            // Class 0 is the fines mode itself and has nowhere to break to.
            const double broken = j == 0 ? 0.0 : mass[j] * selection(grid[j], spec);
            next[j] += mass[j] - broken;
            if (broken <= 0.0) continue;

            // Cell walls fracture at their own scale, independent of the
            // parent, so this share bypasses the breakage function entirely.
            const double to_fines = broken * spec.fines_yield;
            next[0] += to_fines;
            result.cumulative_fines_fraction += to_fines;

            const double to_progeny = broken - to_fines;
            for (std::size_t i = 0; i < j; ++i) {
                next[i] += to_progeny * breakage[j][i];
            }
        }
        mass.swap(next);

        double total = 0.0;
        for (double m : mass) total += m;
        result.mass_balance_residual =
            std::max(result.mass_balance_residual, std::abs(total - 1.0));
    }

    // Drop empty classes, then renormalise over what survives so the emitted
    // distribution satisfies GrindDistribution::validate()'s sum-to-one rule
    // despite the mass floor.
    double kept = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        if (mass[i] > kMassFloor) kept += mass[i];
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (mass[i] <= kMassFloor) continue;
        result.distribution.bins.push_back({units::microns_to_m(grid[i]), mass[i] / kept});
    }

    result.sauter_mean_diameter_um =
        units::m_to_microns(result.distribution.sauter_mean_diameter_m());
    result.geometric_std_dev = result.distribution.geometric_std_dev();
    return result;
}

}  // namespace espressolab
