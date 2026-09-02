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

// Regression: these three fields (simulate's --bean, sweep's
// --workers/--ring-capacity, synthesize's --recipe-path) exist on the CLI
// but were missing from the TUI forms entirely. Their defaults must stay
// empty, matching the CLI's "unset" behaviour (whatever bean the recipe
// carries, sequential sweep execution, provenance = recipe_path).
TEST_CASE("simulate/sweep/synthesize expose the fields the CLI has for them", "[tui][unit]") {
    const auto simulate = default_fields(Command::simulate);
    REQUIRE(field_value(simulate, "bean").empty());

    const auto sweep = default_fields(Command::sweep);
    REQUIRE(field_value(sweep, "workers").empty());
    REQUIRE(field_value(sweep, "ring-capacity").empty());

    const auto synthesize = default_fields(Command::synthesize);
    REQUIRE(field_value(synthesize, "recipe-path").empty());
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

TEST_CASE("parse_optional_ulong treats an empty field as unset, like an absent CLI flag", "[tui][unit]") {
    REQUIRE_FALSE(parse_optional_ulong({{"workers", ""}}, "workers").has_value());
    REQUIRE(parse_optional_ulong({{"workers", "4"}}, "workers") == std::optional<std::size_t>{4});
    try {
        parse_optional_ulong({{"workers", "not-a-number"}}, "workers");
        FAIL("expected InputError");
    } catch (const InputError& error) {
        REQUIRE(error.field == "workers");
    }
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

TEST_CASE("sweep rejects ring-capacity without workers, matching the CLI", "[tui][unit]") {
    std::vector<Field> fields = default_fields(Command::sweep);
    for (auto& field : fields) {
        if (field.label == "ring-capacity") field.value = "8";
    }
    const JobFunction job = make_job(Command::sweep, fields);
    try {
        job({}, {});
        FAIL("expected InputError");
    } catch (const InputError& error) {
        REQUIRE(error.field == "ring-capacity");
    }
}

TEST_CASE("sweep rejects a ring-capacity of zero, matching the CLI", "[tui][unit]") {
    std::vector<Field> fields = default_fields(Command::sweep);
    for (auto& field : fields) {
        if (field.label == "workers") field.value = "2";
        if (field.label == "ring-capacity") field.value = "0";
    }
    const JobFunction job = make_job(Command::sweep, fields);
    try {
        job({}, {});
        FAIL("expected InputError");
    } catch (const InputError& error) {
        REQUIRE(error.field == "ring-capacity");
    }
}

TEST_CASE("simulate threads the bean field through to the shared workflow", "[tui][unit]") {
    // A nonexistent bean path must surface as a load failure rather than
    // being silently ignored -- proof the TUI's "bean" field actually
    // reaches SimulateRequest::bean_path instead of being dropped.
    std::vector<Field> fields = default_fields(Command::simulate);
    use_absolute_baseline_paths(fields);
    for (auto& field : fields) {
        if (field.label == "bean") field.value = "assets/beans/does-not-exist.json";
    }
    const JobFunction job = make_job(Command::simulate, fields);
    REQUIRE_THROWS(job({}, {}));
}

TEST_CASE("append_shot_report shows manifest and region/axial-cell detail", "[tui][unit]") {
    std::vector<Field> fields = default_fields(Command::simulate);
    use_absolute_baseline_paths(fields);
    const JobFunction job = make_job(Command::simulate, fields);
    const JobResult result = job({}, {});

    const auto has = [&](const std::string& needle) {
        return std::any_of(result.lines.begin(), result.lines.end(),
                           [&](const std::string& line) { return line.find(needle) != std::string::npos; });
    };
    REQUIRE(has("coefficient hash"));
    REQUIRE(has("result schema"));
    REQUIRE(has("timestamp"));
    REQUIRE(has("regions ("));
    REQUIRE(has("region 0"));
    REQUIRE(has("cell 0"));

    // Regression: diagnostics.max_flow_m3_s is assigned 1:1 from
    // summary.peak_flow_m3_s -- must not become a second, duplicate line.
    const auto peak_flow_lines =
        std::count_if(result.lines.begin(), result.lines.end(),
                      [](const std::string& line) { return line.find("peak flow") != std::string::npos; });
    REQUIRE(peak_flow_lines == 1);
}

TEST_CASE("run_sweep reports coordinates, warning count, and result hash per run", "[tui][unit]") {
    std::vector<Field> fields = default_fields(Command::sweep);
    for (auto& field : fields) {
        if (field.label == "spec") field.value = (testing::asset_dir() / "sweeps" / "grind-size.json").string();
    }
    const JobFunction job = make_job(Command::sweep, fields);
    // Unlike simulate/cfd/cfd3d, run_sweep calls `progress(...)` unconditionally
    // once per run (tui_forms.cpp), so an empty ProgressCallback (fine for the
    // other commands' job({}, {}) tests above) would throw std::bad_function_call.
    const JobResult result = job({}, [](int, int, std::string) {});

    REQUIRE_FALSE(result.lines.empty());
    const auto run0 = std::find_if(result.lines.begin(), result.lines.end(), [](const std::string& line) {
        return line.rfind("run 0 ", 0) == 0;
    });
    REQUIRE(run0 != result.lines.end());
    REQUIRE(run0->find("puck.particle_diameter_um=") != std::string::npos);
    REQUIRE(run0->find("warnings ") != std::string::npos);
    const auto hash_pos = run0->find("hash ");
    REQUIRE(hash_pos != std::string::npos);
    REQUIRE(run0->size() - (hash_pos + 5) == 16);  // truncated to 16 hex characters
}

TEST_CASE("run_cfd3d reports coefficient hash, schema, and timestamp only when artifacts are written",
         "[tui][unit]") {
    std::vector<Field> fields = default_fields(Command::cfd3d);
    for (auto& field : fields) {
        if (field.label == "recipe") field.value = (testing::asset_dir() / "recipes" / "baseline.json").string();
        if (field.label == "nx") field.value = "6";
        if (field.label == "ny") field.value = "6";
        if (field.label == "nz") field.value = "8";
        if (field.label == "dt") field.value = "0.02";
    }
    const auto has = [](const JobResult& result, const std::string& needle) {
        return std::any_of(result.lines.begin(), result.lines.end(),
                           [&](const std::string& line) { return line.find(needle) != std::string::npos; });
    };

    // No `out`: no artifacts, so no manifest to report -- must not appear.
    const JobResult without_out = make_job(Command::cfd3d, fields)({}, {});
    REQUIRE_FALSE(has(without_out, "coefficient hash"));

    const std::filesystem::path out_dir =
        std::filesystem::temp_directory_path() / "espressolab_tui_forms_cfd3d_manifest_test";
    std::error_code ignored;
    std::filesystem::remove_all(out_dir, ignored);
    for (auto& field : fields) {
        if (field.label == "out") field.value = out_dir.string();
    }
    const JobResult with_out = make_job(Command::cfd3d, fields)({}, {});
    REQUIRE(has(with_out, "coefficient hash"));
    REQUIRE(has(with_out, "result schema"));
    REQUIRE(has(with_out, "timestamp"));
    std::filesystem::remove_all(out_dir, ignored);
}

TEST_CASE("is_recipe_path_field identifies only the recipe-picking fields", "[tui][unit]") {
    REQUIRE(is_recipe_path_field("recipe"));
    REQUIRE(is_recipe_path_field("recipe-path"));
    REQUIRE_FALSE(is_recipe_path_field("coefficients"));
    REQUIRE_FALSE(is_recipe_path_field("bean"));
    REQUIRE_FALSE(is_recipe_path_field("spec"));
}

TEST_CASE("compatible_recipes lists loadable recipes and skips non-recipe files", "[tui][unit]") {
    const std::vector<std::string> found = compatible_recipes(testing::asset_dir() / "recipes");
    REQUIRE_FALSE(found.empty());
    const auto is_baseline = [](const std::string& path) {
        return std::filesystem::path(path).filename() == "baseline.json";
    };
    REQUIRE(std::any_of(found.begin(), found.end(), is_baseline));
    for (const auto& path : found) REQUIRE(std::filesystem::path(path).extension() == ".json");

    REQUIRE(compatible_recipes(testing::asset_dir() / "does-not-exist").empty());
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
