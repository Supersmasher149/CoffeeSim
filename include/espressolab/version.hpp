#pragma once
#include <string_view>

// Every displayed result must be traceable to a named input, an explicit
// equation, and a versioned set of coefficients (guide, "Core engineering
// principle"). These are the version strings stamped into every artifact.
namespace espressolab::version {

inline constexpr std::string_view kSolver = "solver-0.4.0";
inline constexpr std::string_view kRecipeSchema = "1.0";
inline constexpr std::string_view kResultSchema = "1.0";
inline constexpr std::string_view kBeanSchema = "1.0";
// The sensory overlay is versioned independently of kSolver, which is the FIRST
// line of the result_hash byte stream -- bumping that would move the hash of
// every run ever made, including the ones this overlay never touches.
inline constexpr std::string_view kFlavorModel = "flavor-0.1.0";
inline constexpr std::string_view kCfd3dCaseSchema = "1.0";
inline constexpr std::string_view kCfd3dResultSchema = "1.0";
inline constexpr std::string_view kCfd3dFieldFormat = "ELF3D-1";

}  // namespace espressolab::version
