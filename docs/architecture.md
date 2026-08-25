# Architecture

## Dependency rule

Dependencies point inward. `espressolab_core` and `espressolab_models` know
nothing about HTTP, React, plotting or filesystem locations. The same model runs
unchanged in unit tests, the CLI, batch sweeps and the dashboard.

```
web (React/TS)  ->  tool_server (REST)  ->  experiment_runner
                                        ->  artifact_io (JSON/CSV/hashes)
                                                    |
                                        espresso_core (state, stepping, termination)
                                                    |
                                        model_library (water, permeability, heat, extraction)
```

| Target | Owns | Must not own |
| --- | --- | --- |
| `espressolab_models` | Water properties, permeability, heat and extraction correlations | Mutable run state |
| `espressolab_core_types` | Recipe, coefficients, profiles, validation, result types | Solver control flow |
| `espressolab_core` | State variables, stepping, termination, invariants | HTTP, browser UI, plotting |
| `espressolab_artifacts` | Versioned JSON/CSV load and dump, result hashes | Physics decisions |
| `espressolab_experiments` | Sweeps, run schedules, aggregation, artifact naming | Chart rendering |
| `espressolab_calibration` | Measured shots, the loss function, the fit | Anything the solver depends on |
| `espressolab_server` | REST endpoints, jobs and threads, error translation, file boundaries | Equation implementations |
| `web` | Controls, charts, comparisons, warnings, exports | Authoritative calculations |
| `tests/fixtures` | Golden recipes, expected invariants | Production defaults |

`espressolab_core_types` exists so `model_library` can share the domain
vocabulary without linking the solver. It is the one addition to the target list
in section 13.2.

## Runtime data flow

```
Recipe JSON + ModelCoefficients
  -> schema and range validation        (artifact_io, then Recipe::validate)
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

## Threads live in the server, not the engine

`ExperimentRunner::run` takes an optional `(completed, total) -> bool` callback:
it reports progress through it and stops when it returns false. That is the
whole of the engine's involvement in background sweeps. The server owns the
worker thread, the cancellation flag and the job registry, so the same runner
still works unchanged in the CLI and in tests, where there is no thread at all.

## Why the boundary is where it is

The browser never calculates a displayed quantity. It posts a recipe and renders
what comes back, which is why the CLI and the dashboard produce byte-identical
numbers for the same inputs — and why the same solver can be driven by a batch
sweep with no browser in the loop at all.

## Where things live

```
engine/espresso_core/       solver, profiles, validation, domain types
engine/model_library/       water properties, puck resistance, extraction kinetics
engine/artifact_io/         JSON/CSV, SHA-256, manifest stamping
engine/experiment_runner/   sweep axes, cartesian product, aggregation
include/espressolab/        public headers
apps/espressolab_cli/       simulate, sweep, params, version
apps/espressolab_server/    REST endpoints on cpp-httplib
web/src/features/           shot, sweeps, comparison, calibration
assets/                     recipes, coefficients, sweep specs, measured shots
schemas/                    JSON Schema for recipe, coefficients, shot result
tests/                      unit, integration, fixtures
```
