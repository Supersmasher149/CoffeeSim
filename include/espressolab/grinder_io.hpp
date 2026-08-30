#pragma once
#include <filesystem>
#include <string>

#include "espressolab/grinder.hpp"

// Load and dump for the grinder's own documents. Deliberately separate from
// artifact_io's shot formats (3.1): the grinder sits outside the shot pipeline,
// so its schemas, its files, and its versioning are its own and cannot affect a
// shot's artifacts or result hash.
namespace espressolab::grinder_io {

GrinderSpec load_spec_json(const std::string& text);
GrinderSpec load_spec_file(const std::filesystem::path& file);

// The full run: the spec that produced it plus the resulting distribution.
std::string dump_result_json(const GrinderSpec& spec, const GrinderResult& result, int indent = 2);

// Just the `grind` object a recipe's `puck` takes, so the output of a grind can
// be pasted straight into a recipe without hand-editing.
std::string dump_recipe_grind_json(const GrinderResult& result, int indent = 2);

}  // namespace espressolab::grinder_io
