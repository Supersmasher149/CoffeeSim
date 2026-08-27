#pragma once
#include <string_view>

// Every displayed result must be traceable to a named input, an explicit
// equation, and a versioned set of coefficients (guide, "Core engineering
// principle"). These are the version strings stamped into every artifact.
namespace espressolab::version {

inline constexpr std::string_view kSolver = "solver-0.4.0";
inline constexpr std::string_view kRecipeSchema = "1.0";
inline constexpr std::string_view kResultSchema = "1.0";

}  // namespace espressolab::version
