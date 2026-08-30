#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "espressolab/execution.hpp"

// Pure TUI navigation, form, and job-dispatch logic (issues #24, #25, #30).
// Nothing in this header or its implementation touches a terminal or FTXUI,
// so it links into `espressolab_cli_support` alongside the shared workflow
// services and is unit-testable without a TTY. `tui/tui.cpp` is the only
// file that renders it.
namespace espressolab::tui {

// Raised for a field value that fails validation before a workflow runs.
// Mirrors the legacy CLI's (code, path, message) error shape: field label
// stands in for the flag name.
struct InputError final : std::runtime_error {
    InputError(std::string field_name, std::string message)
        : std::runtime_error(std::move(message)), field(std::move(field_name)) {}

    std::string field;
};

struct Field {
    std::string label;
    std::string value;
};

enum class Command {
    simulate,
    sweep,
    calibrate,
    synthesize,
    bench,
    cfd,
    cfd3d,
    grind,
    params,
    fit_params,
    version,
};

struct CommandSpec {
    Command command;
    std::string title;
    std::string help;
};

// Every command reachable from the navigator, in menu order (issue #24: every
// supported command has a discoverable navigator entry).
const std::vector<CommandSpec>& commands();

std::string field_value(const std::vector<Field>& fields, const std::string& label);
std::vector<std::string> split_list(const std::string& text);
std::string format_number(double value, int precision = 2);

double parse_double(const std::vector<Field>& fields, const std::string& label, bool required = true);
int parse_int(const std::vector<Field>& fields, const std::string& label, bool required = true);
bool parse_bool(const std::vector<Field>& fields, const std::string& label);

// The command's existing CLI defaults and units, shown as editable field
// values (issue #24: "preserve the existing command defaults and units").
std::vector<Field> default_fields(Command command);

struct JobResult {
    bool cancelled = false;
    std::vector<std::string> lines;
};

using ProgressCallback = std::function<void(int completed, int total, std::string status)>;
using JobFunction = std::function<JobResult(const CancellationCallback&, const ProgressCallback&)>;

// Builds the job for one command with its captured field values. Every case
// calls the same `cli_workflows` service the legacy CLI command uses
// (issue #25), so results, units, and result hashes match. Field-validation
// failures raise `InputError`; workflow-layer failures propagate
// `espressolab::InvalidInputError` / `espressolab::artifact_io::LoadError`
// / `espressolab::ExecutionCancelled` for the caller to translate.
JobFunction make_job(Command command, std::vector<Field> fields);

}  // namespace espressolab::tui
