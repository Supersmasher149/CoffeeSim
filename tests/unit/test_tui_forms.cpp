#include <catch_amalgamated.hpp>

#include <algorithm>
#include <filesystem>

#include "../fixtures/test_fixtures.hpp"
#include "espressolab/artifact_io.hpp"
#include "espressolab/execution.hpp"
#include "tui/tui_forms.hpp"

// Pure TUI navigation/form coverage (issues #24, #30): every case here runs
// without a terminal, FTXUI, or a TTY -- `tui_forms.cpp` has no dependency on
// either, which is what makes this possible.
using namespace espressolab;
using namespace espressolab::tui;

TEST_CASE("every command has a discoverable navigator entry", "[tui][unit]") {
    // The full command set from CLAUDE.md's CLI surface, minus `tui` itself.
    const std::vector<std::string> expected = {"simulate", "sweep",      "calibrate", "synthesize",
                                               "bench",    "cfd",        "cfd3d",     "grind",
                                               "params",   "fit-params", "version"};
    REQUIRE(commands().size() == expected.size());
    for (const auto& title : expected) {
        const bool found = std::any_of(commands().begin(), commands().end(),
                                       [&](const CommandSpec& spec) { return spec.title == title; });
        INFO("missing navigator entry: " << title);
        REQUIRE(found);
    }
    for (const auto& spec : commands()) REQUIRE(spec.runner != nullptr);
}

TEST_CASE("default_fields preserves existing command defaults and units", "[tui][unit]") {
    const auto simulate = default_fields(Command::simulate);
    REQUIRE(field_value(simulate, "recipe") == "assets/recipes/baseline.json");
    REQUIRE(field_value(simulate, "dt") == "0.01");
    REQUIRE(field_value(simulate, "sample interval") == "0.05");

    // Info commands need no fields: selecting them runs immediately.
    REQUIRE(default_fields(Command::params).empty());
    REQUIRE(default_fields(Command::fit_params).empty());
    REQUIRE(default_fields(Command::version).empty());
}

TEST_CASE("cfd3d defaults leave recipe empty so a loaded case's own recipe is not silently overwritten",
         "[tui][unit]") {
    // Regression: "recipe" used to default to baseline.json here, so
    // run_cfd3d's `if (!request.recipe_path.empty())` fired even when the
    // user never touched the field, discarding a --case file's own recipe.
    // It must stay empty, like the other case-override fields.
    const auto cfd3d = default_fields(Command::cfd3d);
    REQUIRE(field_value(cfd3d, "case").empty());
    REQUIRE(field_value(cfd3d, "recipe").empty());
    REQUIRE(field_value(cfd3d, "coefficients").empty());
}

TEST_CASE("split_list trims whitespace and drops empty entries", "[tui][unit]") {
    REQUIRE(split_list("a, b ,, c") == std::vector<std::string>{"a", "b", "c"});
    REQUIRE(split_list("").empty());
    REQUIRE(split_list("   ").empty());
}

TEST_CASE("parse_double/parse_int identify the offending field on bad input", "[tui][unit]") {
    const std::vector<Field> fields = {{"dt", "not-a-number"}, {"repeats", "3.5"}, {"out", ""}};

    try {
        parse_double(fields, "dt");
        FAIL("expected InputError");
    } catch (const InputError& error) {
        REQUIRE(error.field == "dt");
    }

    try {
        parse_int(fields, "repeats");
        FAIL("expected InputError");
    } catch (const InputError& error) {
        REQUIRE(error.field == "repeats");
    }

    // An optional, empty field is not an error.
    REQUIRE(parse_double(fields, "out", /*required=*/false) == 0.0);

    // A required, empty field still names itself.
    try {
        parse_double(fields, "out", /*required=*/true);
        FAIL("expected InputError");
    } catch (const InputError& error) {
        REQUIRE(error.field == "out");
    }
}

TEST_CASE("parse_bool accepts the documented spellings only", "[tui][unit]") {
    REQUIRE(parse_bool({{"x", "true"}}, "x"));
    REQUIRE(parse_bool({{"x", "yes"}}, "x"));
    REQUIRE(parse_bool({{"x", "1"}}, "x"));
    REQUIRE_FALSE(parse_bool({{"x", "false"}}, "x"));
    REQUIRE_FALSE(parse_bool({{"x", ""}}, "x"));
    REQUIRE_THROWS_AS(parse_bool({{"x", "maybe"}}, "x"), InputError);
}

TEST_CASE("make_job runs the same native workflow the legacy CLI uses", "[tui][unit]") {
    // `version` needs no fields and no filesystem access, so it is a fast,
    // deterministic check that the TUI's dispatch reaches the shared
    // workflow layer and reports its result as plain lines.
    const JobFunction job = make_job(Command::version, {});
    const JobResult result = job({}, {});
    REQUIRE_FALSE(result.cancelled);
    REQUIRE(result.lines.size() == 1);
    REQUIRE(result.lines.front().find("recipe-schema=") != std::string::npos);
}

TEST_CASE("make_job surfaces a bad field as InputError before touching the workflow", "[tui][unit]") {
    std::vector<Field> fields = default_fields(Command::simulate);
    for (auto& field : fields) {
        if (field.label == "dt") field.value = "not-a-number";
    }
    const JobFunction job = make_job(Command::simulate, fields);
    REQUIRE_THROWS_AS(job({}, {}), InputError);
}

// Regression: default_fields(Command::simulate)'s "recipe" and
// "coefficients" values are the CWD-relative "assets/recipes/baseline.json"
// and "assets/coefficients/default-v1.json" -- deliberate defaults for
// interactive TUI use (the CLI's own usage examples assume the same), not
// paths this test suite can rely on. Catch2 binaries are meant to run from
// anywhere (ctest's working directory is the build tree, not the repo
// root), so both cancellation tests below override those two fields with
// absolute paths the same way tests/unit/test_cli_workflows.cpp does,
// instead of depending on the process's current directory.
void use_absolute_baseline_paths(std::vector<Field>& fields) {
    for (auto& field : fields) {
        if (field.label == "recipe") field.value = (testing::asset_dir() / "recipes" / "baseline.json").string();
        if (field.label == "coefficients") {
            field.value = (testing::asset_dir() / "coefficients" / "default-v1.json").string();
        }
    }
}

TEST_CASE("make_job honours cooperative cancellation", "[tui][unit][cancellation]") {
    std::vector<Field> fields = default_fields(Command::simulate);
    use_absolute_baseline_paths(fields);
    const JobFunction job = make_job(Command::simulate, fields);
    REQUIRE_THROWS_AS(job([] { return true; }, {}), ExecutionCancelled);
}

TEST_CASE("a cancelled simulate job never reaches the artifact writer", "[tui][unit][cancellation]") {
    const std::filesystem::path out_dir =
        std::filesystem::temp_directory_path() / "espressolab_tui_forms_cancel_test";
    std::error_code ignored;
    std::filesystem::remove_all(out_dir, ignored);

    std::vector<Field> fields = default_fields(Command::simulate);
    use_absolute_baseline_paths(fields);
    for (auto& field : fields) {
        if (field.label == "out") field.value = out_dir.string();
    }
    const JobFunction job = make_job(Command::simulate, fields);
    REQUIRE_THROWS_AS(job([] { return true; }, {}), ExecutionCancelled);
    REQUIRE_FALSE(std::filesystem::exists(out_dir));

    std::filesystem::remove_all(out_dir, ignored);
}

TEST_CASE("calibrate rejects leave-one-out combined with holdout, matching the CLI", "[tui][unit]") {
    std::vector<Field> fields = default_fields(Command::calibrate);
    for (auto& field : fields) {
        if (field.label == "leave-one-out") field.value = "true";
        if (field.label == "holdout") field.value = "some-shot";
    }
    const JobFunction job = make_job(Command::calibrate, fields);
    try {
        job({}, {});
        FAIL("expected InputError");
    } catch (const InputError& error) {
        REQUIRE(error.field == "holdout");
    }
}
