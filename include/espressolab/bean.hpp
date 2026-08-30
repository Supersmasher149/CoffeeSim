#pragma once
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "espressolab/validation.hpp"

namespace espressolab {

// A bean profile: what the coffee is made of, in the only terms this model can
// act on. It carries no brew controls -- dose, grind and profiles stay in the
// Recipe -- and no physics: the solver's mass, TDS and yield are identical with
// and without one. See docs/model.md, "Sensory overlay".
//
// Every number in a bean document is an AUTHORED PRIOR. None has been measured,
// fitted, or checked against a chromatogram or a tasting panel. The shape is
// grounded in the standard qualitative account of roast chemistry; the values
// are chosen to produce plausible relative behaviour and nothing more.

// A closed vocabulary rather than free strings: the axis-weight matrix is dense,
// validation is total (every class has a weight on every axis), and a typo in a
// bean file becomes a load error instead of a silently-zero column.
enum class SoluteClass {
    acids,        // chlorogenic, citric, malic -- first out of the puck
    sugars,       // caramelised carbohydrates
    maillard,     // melanoidins; the chocolate/nut/caramel register
    lipids,       // the oils and colloids that carry body
    bitter,       // late-eluting roast degradation products
    polyphenols,  // tannins; the drying, woody tail
};
inline constexpr std::size_t kSoluteClassCount = 6;

const char* to_string(SoluteClass value);
bool solute_class_from_string(const std::string& name, SoluteClass& out);

enum class SensoryAxis {
    fruit,
    acidity,
    sweetness,
    chocolate,
    body,
    bitterness,
    astringency,
};
inline constexpr std::size_t kSensoryAxisCount = 7;

const char* to_string(SensoryAxis value);
bool sensory_axis_from_string(const std::string& name, SensoryAxis& out);

// One solute class's share of the extractable solids, and how fast it leaves
// relative to the `maillard` reference class (1.0 by definition).
//
// `relative_rate` is a ratio, never an absolute rate constant: the overlay
// partitions the mass the solver already extracted rather than running its own
// kinetics, so only the ladder between classes matters. That is what keeps the
// lumped total -- and therefore every existing number -- untouched.
struct SoluteClassShare {
    double mass_fraction = 0.0;  // of extractable solids; the six sum to 1
    double relative_rate = 1.0;  // multiplier on extraction propensity
};

// Row-major [axis][class]. Only each row's *shape* matters: axis_intensities()
// normalises by the row's own maximum, so a hand-edited bean cannot saturate an
// axis by writing large numbers.
using AxisWeightMatrix =
    std::array<std::array<double, kSoluteClassCount>, kSensoryAxisCount>;

// The cup the roaster says the coffee should land in, on the same 0-10 scale the
// model reports. This is what makes the verdict bean-relative: "sour for this
// bean's declared target", never "sour" against a universal ideal.
struct SensoryTargetAxis {
    double intensity = 0.0;  // 0-10
    double tolerance = 1.5;  // intensity points that count as one unit off
    double weight = 1.0;     // 0 excludes the axis from the match score
};
using SensoryTarget = std::array<SensoryTargetAxis, kSensoryAxisCount>;

// Descriptive only, and deliberately erased before recipe_hash() -- the same
// rule and the same reason as CoefficientProvenance (types.hpp) and its erasure
// in hashing.cpp: editing a roaster's cupping note must not change what a run
// means or which artifact directory it lands in.
struct BeanDescription {
    std::string roaster;
    std::string display_name;
    std::string roast_level;               // "light" | "medium" | "dark"
    std::vector<std::string> origins;      // e.g. "70% Guatemala CODECH washed"
    std::vector<std::string> notes;        // the roaster's published notes
    std::optional<double> suggested_ratio; // e.g. 17.0 for a 1:17 filter ratio
    std::string source;                    // where these numbers came from
    std::vector<std::string> limitations;
};

struct BeanProfile {
    std::string schema_version = "1.0";
    std::string id = "unnamed-bean";
    std::string version = "1.0.0";

    std::array<SoluteClassShare, kSoluteClassCount> classes{};
    AxisWeightMatrix axis_weights{};
    SensoryTarget target{};

    std::optional<BeanDescription> description;

    [[nodiscard]] ValidationResult validate() const;
};

// The class ladder and axis-weight matrix shared by every shipped bean: these
// are properties of the *model*, not of any one coffee (assets/beans/README.md
// says so too). A bean document may override them, but none of the three in the
// catalogue does -- they differ only in `classes[].mass_fraction` and `target`.
[[nodiscard]] std::array<double, kSoluteClassCount> default_relative_rates();
[[nodiscard]] AxisWeightMatrix default_axis_weights();

}  // namespace espressolab
