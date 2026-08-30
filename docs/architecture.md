# Architecture

## Dependency rule

Dependencies point inward. `espressolab_core` and `espressolab_models` know
nothing about HTTP, React, plotting or filesystem locations. The same model runs
unchanged in unit tests, the CLI, batch sweeps and the dashboard.

```
web (React/TS)  ->  tool_server (REST)  ->  experiment_runner
                                         ->  artifact_io (JSON/CSV/hashes)
                                         ->  reference_io (published metadata)
                                                     |
                                         espresso_core (state, stepping, termination)
                                                     |
                                         model_library (water, permeability, heat, extraction)

CLI and CFD tests  ->  cfd (separate Level 4 axisymmetric solver) -> espresso_core + model_library
CLI and REST tests ->  cfd3d (Level 4b Cartesian solver)          -> espresso_core + model_library

espressolab_cli grind  ->  grind (separate comminution model)
                                      -> espressolab_core_types only

espressolab_cli tui  ->  espressolab_cli_support (workflows.cpp, tui/tui_forms.cpp)
                                                     |
                                    same call every legacy command_* handler makes
```

| Target | Owns | Must not own |
| --- | --- | --- |
| `espressolab_models` | Water properties, permeability, heat and extraction correlations | Mutable run state |
| `espressolab_core_types` | Recipe, coefficients, profiles, validation, result types | Solver control flow |
| `espressolab_core` | State variables, stepping, termination, invariants | HTTP, browser UI, plotting |
| `espressolab_artifacts` | Versioned JSON/CSV load and dump, result hashes | Physics decisions |
| `espressolab_experiments` | Sweeps, run schedules, aggregation, artifact naming | Chart rendering |
| `espressolab_calibration` | Measured shots, the loss function, the fit | Anything the solver depends on |
| `espressolab_grind` | Separate comminution model (burr geometry -> particle size distribution) and its own spec/result documents | Recipes, shot artifacts, the default pipeline, any result hash |
| `espressolab_cfd` | Separate Level 4 axisymmetric pressure and transport solver | REST, dashboard, standard artifacts, default result hashes |
| `espressolab_cfd3d` | Level 4b Cartesian 3D REV-scale pressure and transport solver | Default Level 1-3 path, equation ownership outside the solver |
| `espressolab_references` | Published reference-shot metadata and partial-load reporting | Simulation, fitting, or validation decisions |
| `espressolab_server` | REST endpoints, jobs and threads, error translation, file boundaries | Equation implementations |
| `espressolab_cli_support` | Shared CLI workflow services (load, validate, run, write artifacts) and pure TUI navigation/form logic, called by both the legacy commands and the TUI | FTXUI, terminal I/O, argv parsing |
| `apps/espressolab_cli/tui` | FTXUI rendering, input handling, the one-job-at-a-time worker thread | Physics, artifact formats, validation rules (all in `espressolab_cli_support`) |
| `web` | Controls, charts, comparisons, warnings, exports | Authoritative calculations |
| `tests/fixtures` | Golden recipes, expected invariants | Production defaults |

`espressolab_core_types` exists so `model_library` can share the domain
vocabulary without linking the solver. It is the one addition to the target list
in section 13.2.

`espressolab_cfd` is deliberately outside the default Level 1-3 request path.
The CLI invokes it directly and its verification tests exercise it separately.
It must not change the dashboard response, standard artifact shape, or the
result hashes emitted by the default solver.

`espressolab_cfd3d` is also isolated from the default path. The CLI and the
dedicated REST endpoints invoke it explicitly. Its case, summary, snapshot
fields, and `ELF3D-1` field artifacts are separate contracts and never alter
the standard shot result or its hashes.

`espressolab_grind` is isolated in the same way, and more strictly: it depends
on `espressolab_core_types` alone, has no REST route at all, and its spec and
result documents carry no recipe hash, no coefficient hash, and no result hash.
It produces a distribution a recipe *may* be given by hand (`puck.grind`), which
is the only coupling between the two, and it is a copy-paste boundary rather
than a call: nothing in the shot pipeline invokes the grinder.

`espressolab_cli tui` is an application-layer frontend: it calls
`espressolab_cli_support` directly (native loaders, solvers, calibration APIs,
experiment runner, artifact writers), never the REST server and never the
file-oriented CLI itself. FTXUI is a private dependency of the
`espressolab_cli` executable only -- `espressolab_cli_support` links the same
engine libraries as every other target above and has no terminal UI
dependency, so it stays usable from `espressolab_tests` (and could, in
principle, back a future non-terminal frontend) without pulling FTXUI along.

## Runtime data flow

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

The explicit 3D path uses the same recipe and coefficient boundaries, then
builds a circular Cartesian cut-cell mesh, runs the Level 4b solver, and emits
`Cfd3dResult` plus optional time-indexed field snapshots.
```

## Threads live in the applications, not the engine

`ExperimentRunner::run` takes an optional `(completed, total) -> bool` callback:
it reports progress through it and stops when it returns false. That is the
whole of the engine's involvement in background sweeps. Three application-layer
places own threads, and no engine target owns any:

| Owner | Thread it owns | Purpose |
| --- | --- | --- |
| `apps/espressolab_server/` | Job threads | Background REST sweeps and CFD3D runs |
| `apps/espressolab_cli/tui/` | One worker thread | Keeps the render loop responsive during a job |
| `apps/espressolab_cli/sweep_batch_runner.cpp` | A pool of worker threads | `sweep --workers`, the parallel batch path |

The third is the newest and the easiest to misread. `sweep_batch_runner.cpp`
is the only place in the codebase that runs sweep points concurrently;
`engine/experiment_runner/` stays single-threaded and knows nothing about it.
The two paths share the pure, stateless building blocks declared in
`experiment.hpp` (`execute_sweep_point` and friends), which is what lets them
produce byte-identical results: worker count and ring capacity change arrival
order at the consumer, never a run's content or its final position in
`result.runs`. A `BoundedRingBuffer`
(`include/espressolab/ring_buffer.hpp`) decouples the producer threads from
the consumer that aggregates them, so a slow consumer applies back-pressure
instead of growing an unbounded queue. Leaving `--workers` unset keeps the
sequential path exactly as it was.

Because the engine owns no thread, the same runner still works unchanged in
the CLI, the TUI, and in tests, where there is no thread at all.

Native execution beyond sweeps is thread-agnostic the same way: `Simulator`,
`CfdSolver`, `Cfd3dSolver`, and `calibration::fit`/`leave_one_out` each accept
an optional `espressolab::CancellationCallback` (`include/espressolab/execution.hpp`),
checked at safe solver, pressure-iteration, calibration, and sweep boundaries.
A cancelled call throws `ExecutionCancelled` rather than returning a partial
result, which is what lets a caller guarantee it never writes an incomplete
artifact -- it simply never reaches the write step. These controls are
deliberately outside recipe, coefficient, configuration, and result hashes:
whether a run was cancelled changes nothing about what the run *meant*.

## Why the boundary is where it is

The browser never calculates a displayed quantity. It posts a recipe and renders
what comes back, which is why the CLI and the dashboard produce byte-identical
numbers for the same inputs — and why the same solver can be driven by a batch
sweep with no browser in the loop at all.

## Where things live

```
engine/espresso_core/       solver, profiles, validation, domain types
engine/model_library/       water properties, puck resistance, extraction kinetics
engine/cfd/                 separate Level 4 axisymmetric CFD solver
engine/cfd3d/               Level 4b Cartesian 3D CFD solver
engine/grind/               separate comminution model and its own IO
engine/artifact_io/         JSON/CSV, SHA-256, manifests, ELF3D fields
engine/experiment_runner/   sweep axes, cartesian product, aggregation (single-threaded)
engine/reference_io/        reference-shot catalogue loader
include/espressolab/        public headers, execution.hpp, ring_buffer.hpp
apps/espressolab_cli/       simulate, sweep, params, version, calibrate, synthesize,
                            bench, cfd, cfd3d, grind; sweep_batch_runner.{hpp,cpp}
                            is the parallel sweep path behind --workers
apps/espressolab_cli/tui/   interactive terminal UI over the same shared workflow services
apps/espressolab_server/    REST endpoints on cpp-httplib
web/src/features/           shot, sweeps, comparison, calibration
assets/                     recipes, coefficients, sweep specs, measured shots, grinder specs
schemas/                    JSON Schema for recipe, coefficients, shot result, CFD3D cases/results,
                            grinder specs/results
tests/                      unit, integration, fixtures; pty/, server/, cli/, schemas/ run outside test.sh
```
