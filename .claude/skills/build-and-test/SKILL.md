---
name: build-and-test
description: Builds the native EspressoLab targets and runs the Catch2 test suite, including single-tag runs, the isolated Debug build, the warnings-as-errors build, the TUI PTY smoke matrix, and the web typecheck/build checks. Use when asked to build the project, run tests, run one test tag while iterating, or verify a change compiles and passes before committing.
---

# Build and Test

## When to Use

- Verifying a native or web change builds and passes its tests
- Running the full suite before opening/finishing a PR
- Running a single Catch2 tag while iterating on one area
- Checking the warnings-as-errors build still compiles clean

## Standard Build and Test

- `./scripts/build.sh [Debug|Release]` — cmake configure + build into `build/` (Release default), all deps vendored for an offline build
- `./scripts/test.sh` — runs `build.sh` then the full Catch2 suite (`build/tests/espressolab_tests`); does **not** include the PTY smoke test, web checks, `demo.sh`, or `calibration_demo.sh`
- Current suite size: 165 cases / 18,213 assertions (informational baseline, will drift — don't hardcode expectations against it)

## Running a Single Tag

- Build first, then: `./build/tests/espressolab_tests "[tag]"`
- List all available tests: `./build/tests/espressolab_tests --list-tests`
- Full tag list: `[unit] [units] [profile] [water] [permeability] [flow] [heat] [extraction] [artifacts] [integration] [invariants] [convergence] [sweep] [calibration] [recovery] [property] [performance] [regions] [axial] [cfd] [cfd3d] [verification] [references] [progress] [cancellation] [tui] [cli_workflows] [grind] [grind_sim]`
- Match the tag to the touched area, e.g. CFD solver changes → `[cfd]`/`[cfd3d]`, TUI navigation logic → `[tui]`/`[cli_workflows]`, comminution model → `[grind]`/`[grind_sim]`

## Debug Build (isolated from `build/`)

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j4
ctest --test-dir build-debug --output-on-failure
```

Use this when diagnosing a native failure that needs a debugger or unoptimized symbols.

## Warnings-as-Errors Build

```bash
cmake -S . -B build-warnings -DESPRESSOLAB_WARNINGS_AS_ERRORS=ON
cmake --build build-warnings -j4
```

CI's hosted native job always builds with this flag on — run it locally before a PR that touches headers, templates, or anything warning-prone.

## TUI PTY Smoke Test (separate from ctest)

- `python3 tests/pty/tui_smoke.py [path/to/espressolab_cli]`
- Needs a real POSIX pseudo-terminal, not just the Catch2 binary; run after any change to `apps/espressolab_cli/tui/`
- Defines 14 checks; fails with a diagnostic (not a crash) when its prerequisites (built binary, PTY-capable environment) are missing

## Web Checks (separate, no browser-interaction harness)

```bash
npm --prefix web run typecheck
npm --prefix web run build
```

These only check types and that the app compiles — no automated test exercises dashboard interactions; see the `dev-server` skill for manual verification.

## Performance Budget

- `./build/apps/espressolab_cli/espressolab_cli bench --seconds <s> --repeats <n>` (CI runs `--repeats 50`) — run when touching hot solver loops

## Related Skills

- `acceptance-demo` for the determinism/reproducibility check (not part of `test.sh`)
- `pr-checklist` links here for its "run the smallest relevant checks" item
- `data-contract-change` (step 5, tests) and `grind-cfd-workflow`/`calibration-workflow` point here for their relevant tags
