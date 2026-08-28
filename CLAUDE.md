# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

EspressoLab is a local engineering workbench that simulates an espresso shot
from controllable brew inputs. Deterministic C++20 simulation core, React/TypeScript
dashboard, an interactive terminal UI (`espressolab_cli tui`), no AI, plus a
separate experimental CFD solver. Neither the browser nor the TUI calculates a
displayed quantity — both post/call into the same native workflows and render
what the native solver returns, so the file-oriented CLI, the TUI, tests, and
the dashboard produce byte-identical numbers for the same inputs.

## Commands

```bash
./scripts/build.sh          # cmake + build Release into build/ (deps vendored, offline)
./scripts/test.sh           # build + run the full Catch2 suite (not PTY/web/demo)
./scripts/demo.sh           # acceptance run: baseline shot, grind sweep, determinism check
./scripts/dev.sh            # tool server (port 8734) + dashboard dev server (localhost:5173)
```

Run a single test tag or list tests (build first):

```bash
./build/tests/espressolab_tests "[cfd]"
./build/tests/espressolab_tests --list-tests
```

Tags: `[unit]` `[units]` `[profile]` `[water]` `[permeability]` `[flow]` `[heat]`
`[extraction]` `[artifacts]` `[integration]` `[invariants]` `[convergence]`
`[sweep]` `[calibration]` `[recovery]` `[property]` `[performance]` `[regions]`
`[axial]` `[cfd]` `[cfd3d]` `[verification]` `[references]` `[progress]`
`[cancellation]` `[tui]` `[cli_workflows]`.

A POSIX PTY smoke matrix for the TUI runs separately from `espressolab_tests`
(it needs a real pseudo-terminal, not just the Catch2 binary):

```bash
python3 tests/pty/tui_smoke.py [path/to/espressolab_cli]
```

The current Catch2 suite passes 165 cases/18,213 assertions. The PTY script
defines 14 checks but was not rerun for the measured-shot documentation refresh.
Hosted macOS, Linux, and dashboard jobs passed at `736cef3`; later hardening
through `94fbe7a` does not yet have equivalent hosted evidence.

Debug build, isolated from `build/`:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j4
ctest --test-dir build-debug --output-on-failure
```

Warnings-as-errors build (passes as of the TUI work in #23; the shadowing
diagnostic this note used to describe was resolved by the `simulator.cpp`
puck-region refactor):

```bash
cmake -S . -B build-warnings -DESPRESSOLAB_WARNINGS_AS_ERRORS=ON
cmake --build build-warnings -j4
```

Web checks (no browser interaction test harness exists — test dashboard
interactions manually via `./scripts/dev.sh`):

```bash
npm --prefix web run typecheck
npm --prefix web run build
```

CLI surface:

```bash
espressolab_cli simulate --recipe <file> [--coefficients <file>] [--out <dir>]
espressolab_cli sweep    --spec <file> [--out <dir>]
espressolab_cli calibrate --shots <dir> --fit <name,...> [--holdout <id,...>] [--leave-one-out]
espressolab_cli synthesize --recipe <file> [--noise <g>] --out <file>
espressolab_cli cfd      --recipe <file> [--radial <n>] [--axial <n>] [--field pressure|saturation|temperature|tds]
espressolab_cli cfd3d    --recipe <file> [--nx <n>] [--ny <n>] [--nz <n>] [--out <dir>]
espressolab_cli bench    [--seconds <s>] [--repeats <n>]
espressolab_cli params | fit-params | version
espressolab_cli tui      # interactive terminal UI (POSIX TTY only)
```

## Architecture

Dependencies point strictly inward. `espressolab_core` and `espressolab_models`
know nothing about HTTP, React, plotting, or filesystem locations — the same
model runs unchanged in unit tests, the CLI, headless sweeps, and the browser.

```
web (React/TS)  ->  tool_server (REST)  ->  experiment_runner
                                         ->  artifact_io (JSON/CSV/hashes)
                                         ->  reference_io (published metadata)
                                                     |
                                         espresso_core (state, stepping, termination)
                                                     |
                                         model_library (water, permeability, heat, extraction)

CLI and CFD tests  ->  cfd / cfd3d (separate Level 4 solvers)
                                      -> espresso_core + model_library

espressolab_cli tui  ->  espressolab_cli_support (workflows.cpp, tui/tui_forms.cpp)
                                                     |
                            same call as every legacy command_* handler above
```

| Target | Owns | Must not own |
| --- | --- | --- |
| `espressolab_models` | Water properties, permeability, heat and extraction correlations | Mutable run state |
| `espressolab_core_types` | Recipe, coefficients, profiles, validation, result types | Solver control flow |
| `espressolab_core` | State variables, stepping, termination, invariants | HTTP, browser UI, plotting |
| `espressolab_artifacts` | Versioned JSON/CSV load and dump, result hashes | Physics decisions |
| `espressolab_experiments` | Sweeps, run schedules, aggregation, artifact naming | Chart rendering |
| `espressolab_calibration` | Measured shots, the loss function, the fit | Anything the solver depends on |
| `espressolab_cfd` | Separate Level 4 axisymmetric pressure/transport solver | REST, dashboard, standard artifacts, default result hashes |
| `espressolab_cfd3d` | Separate Cartesian 3D pressure/transport solver and field snapshots | Standard shot artifacts and default result hashes |
| `espressolab_references` | Published reference-shot metadata, partial-load reporting | Simulation, fitting, validation decisions |
| `espressolab_server` | REST endpoints, jobs and threads, error translation, file boundaries | Equation implementations |
| `espressolab_cli_support` | Shared CLI workflow services (load, validate, run, write artifacts) and pure TUI navigation/form logic, called by both the legacy commands and the TUI | FTXUI, terminal I/O, argv parsing |
| `apps/espressolab_cli/tui` | FTXUI rendering, input handling, the one-job-at-a-time worker thread | Physics, artifact formats, validation rules (all in `espressolab_cli_support`) |
| `web` | Controls, charts, fixed-coefficient measured-shot comparisons, warnings, exports | Authoritative calculations or coefficient fitting |
| `tests/fixtures` | Golden recipes, expected invariants | Production defaults |

Keep a new concern in the lowest layer that can own it: physics in the model
library/solver, serialization in `artifact_io`, request translation in the
server, rendering-only behavior in `web/`. The CFD solvers are deliberately
outside the default Level 1-3 request path. CFD3D has explicit REST routes and
independent artifacts/schemas; neither CFD solver may become a hidden dependency
of the default pipeline or change its artifacts/hashes.

Threads live in the server and the TUI, not the engine: `ExperimentRunner::run`
takes a `(completed, total) -> bool` progress callback and stops when it
returns false. That's the engine's whole involvement in background sweeps —
the server (for REST) and `apps/espressolab_cli/tui` (for the interactive
shell) each own their own worker thread, cancellation flag, and job registry,
so the same runner and the same native solver/calibration APIs work unchanged
in the CLI, the TUI, and in tests (no thread at all there). Native execution
stays thread-agnostic: cancellation and coarse progress are a
`CancellationCallback`/status-callback pair (`include/espressolab/execution.hpp`)
checked at safe solver, pressure-iteration, calibration, and sweep boundaries,
never a thread the solver owns itself. These controls are deliberately outside
recipe, coefficient, configuration, and result hashes.

### Repository map

| Location | Responsibility |
| --- | --- |
| `engine/espresso_core/` | Domain types, validation, profiles, state stepping, termination, invariants for the Level 1-3 solver |
| `engine/model_library/` | Water properties, puck geometry/permeability, heat, extraction correlations |
| `engine/cfd/` | Separate Level 4 axisymmetric porous-media solver |
| `engine/cfd3d/` | Separate Cartesian 3D porous-media solver and field storage |
| `engine/artifact_io/` | JSON/CSV load and dump, canonical hashes, manifests, artifact files |
| `engine/experiment_runner/` | Sweep axes, Cartesian execution, progress, aggregate export |
| `engine/calibration/` | Measured-shot loading, loss functions, fitting, validation reports |
| `engine/reference_io/` | Read-only reference-shot catalogue loading |
| `include/espressolab/` | Public C++ headers shared across targets, including `execution.hpp` (the cancellation/status contract) |
| `apps/espressolab_cli/` | Argv parsing, `workflows.{hpp,cpp}` (shared CLI workflow services), file-oriented command output |
| `apps/espressolab_cli/tui/` | Interactive terminal UI: `tui_forms.{hpp,cpp}` (terminal-independent navigation/forms) and `tui.cpp` (FTXUI rendering) |
| `apps/espressolab_server/` | Local REST translation, measured-shot comparison, in-memory runs, background sweep/CFD3D jobs (cpp-httplib) |
| `web/src/features/` | shot, sweeps, run comparison, measured-shot comparison, calibration notice |
| `assets/` | Versioned example inputs and synthetic measurement fixtures |
| `schemas/` | Intended JSON exchange formats |
| `tests/` | Unit, integration, property, convergence, verification tests |
| `tests/pty/` | POSIX PTY smoke matrix for the TUI (outside `ctest`; needs a real pseudo-terminal) |

### Runtime data flow

```
Recipe JSON + ModelCoefficients
  -> document and range validation      (artifact_io, then Recipe::validate)
  -> profile preprocessing              (precomputed segment slopes)
  -> initial puck and water state
  -> fixed-step solver loop
       pressure / inlet-temperature boundary lookup
       water properties at puck temperature
       compression, porosity, permeability, resistance
       flow, saturation, retained liquid
       heat transfer
       extraction into pore liquid, transport into the cup
       warnings and sample recording
  -> ShotResult + time series + hashes
  -> REST response / JSON / CSV
  -> React dashboard
```

## Model fidelity

A lateral parallel-region puck divided into stacked axial finite-volume cells:
Darcy flow through a Kozeny-Carman-shaped permeability with cells in hydraulic
series, a bounded empirical compression curve, cell-local thermal and
extraction states. `axial_cells` defaults to 1 (the lumped puck). Every
equation is in `docs/model.md`.

- No radial structure or dynamic channelling in the default pipeline — the
  separate CFD solver (`engine/cfd/`) adds the radial coordinate but does not
  feed the dashboard or the default artifacts.
- No pore-resolved simulation anywhere; the CFD closure is Darcy at the
  representative-elementary-volume scale.
- Nothing is validated against a real shot. Default coefficients are
  uncalibrated (plausible baseline, not fitted). `espressolab_cli calibrate`
  exists and is tested but has only run against synthetic data — see
  `docs/calibration.md`.
- The measured-shot catalogue/compare API evaluates one stored shot with one
  simulation and fixed coefficients. It is not a fitting path, and the current
  stored shots are synthetic fixtures.
- No grinder-dial-to-particle-size mapping; grind is a physical input in µm.
- No flavour prediction — TDS/extraction are engineering outputs only.

## Data contracts

The C++ loaders/serializers in `artifact_io` are the executable contract, not
`schemas/` (which documents intended formats but isn't automatically enforced
at runtime). When schema text and runtime behavior differ, the loader/serializer
is correct and the mismatch is a bug to fix.

Units convert only at the recipe/result boundary; internals are SI:

| Quantity | External unit | Internal unit |
| --- | --- | --- |
| Dose and beverage mass | g | kg |
| Basket diameter and puck depth | mm | m |
| Particle diameter | µm | m |
| Pressure | bar | Pa |
| Temperature | °C | K |
| Flow | ml/s | m³/s |
| TDS and extraction yield | percent | fraction |

Never add browser-side formulae for authoritative metrics — the dashboard may
derive display-only presentation values from samples, but must render model
outputs as returned by the native solver (e.g. the cross-section spout
differences beverage mass between samples for display timing; the sampled flow
field is the Darcy flow into the bed, a different number until pores are full).

Changing a data contract (recipe, coefficients, result, sweep, measured shot,
comparison, CFD3D, reference) must
be done atomically across layers — see the "Changing a Data Contract" and
"Contract Change Procedure" checklists in `docs/development.md` and
`docs/data-contracts.md`. In short: C++ domain type -> loader/serializer/hash
-> JSON schema + REST docs -> TypeScript types + UI -> tests -> docs.

## Reproducibility

Every run records recipe hash, coefficient hash, solver version, step size,
and a SHA-256 result hash over canonicalized inputs and ordered output samples.
Identical inputs must produce identical hashes (`scripts/demo.sh` checks this).
Coefficient sets are versioned separately from recipes and solver code so a
re-fit never silently changes what a past run meant. Changing serialization
order or the set of hashed fields is a deliberate, tested contract change.

There is deliberately no exact-snapshot golden test — invariants, mass
balances, and the convergence test guard correctness; the result hash is for
reproducibility, not correctness.

## Pull-request checklist

- Keep dependency direction intact; no UI/HTTP decisions in the solver.
- Preserve deterministic ordering, canonical serialization, and result hashes
  unless a versioned compatibility change is intended.
- Validate finite numeric inputs before they reach a numerical loop.
- Add a regression test for every fixed defect, especially malformed input or
  an invariant violation.
- Run the smallest relevant native, web, and documentation checks before review.
- State whether a change is verified by a test, only manually exercised, or
  not yet validated against real measurements.

## Documentation

| Document | Contents |
| --- | --- |
| `docs/getting-started.md` | Build, test, simulate, run the dashboard, locate artifacts |
| `docs/development.md` | Contributor workflow, module ownership, build variants, quality checks |
| `docs/data-contracts.md` | Recipe, coefficient, result, sweep, measured-shot, CFD3D, reference data ownership |
| `docs/model.md` | Every equation, coefficient, and guardrail |
| `docs/architecture.md` | Component boundaries and the dependency rule |
| `docs/api.md` | REST endpoints and the error contract |
| `docs/calibration.md` | Fitting coefficients to measured shots |
| `docs/testing.md` | What each test layer is for |
| `docs/roadmap.md` | Status against the four-week plan, and what's not done |
| `docs/current-state-and-gaps.md` | Evidence-based implementation status and open gaps |
| `schemas/` | JSON Schema for recipes, coefficients, shot results, CFD3D cases/results |

## Requirements

CMake 3.20+, a C++20 compiler, Node 20+ for the dashboard. nlohmann/json,
Catch2, cpp-httplib, and FTXUI (the CLI's TUI, linked only by `espressolab_cli`)
are vendored in `third_party/`, so a clean clone builds offline. The TUI itself
targets interactive POSIX terminals (macOS and Linux) and rejects non-TTY
stdin/stdout with a stable exit code rather than entering the render loop.
