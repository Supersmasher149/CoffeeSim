# Development Guide

This guide defines how to work on EspressoLab without breaking its dependency
boundaries, reproducibility guarantees, or cross-language data contracts.

## Repository Map

| Location | Responsibility |
| --- | --- |
| `engine/espresso_core/` | Domain types, validation, profiles, state stepping, termination, and invariants for the Level 1-3 solver |
| `engine/model_library/` | Water properties, puck geometry and permeability, heat, and extraction correlations |
| `engine/cfd/` | Separate Level 4 axisymmetric porous-media solver |
| `engine/artifact_io/` | JSON/CSV load and dump, canonical hashes, manifests, and artifact files |
| `engine/experiment_runner/` | Sweep axes, Cartesian execution, progress, and aggregate export |
| `engine/calibration/` | Measured-shot loading, loss functions, fitting, and validation reports |
| `engine/reference_io/` | Read-only reference-shot catalogue loading |
| `include/espressolab/` | Public C++ headers shared across targets |
| `apps/espressolab_cli/` | Command-line parsing and file-oriented workflows |
| `apps/espressolab_server/` | Local REST translation, in-memory runs, and background sweep jobs |
| `web/src/` | React controls and visualizations for server-provided results |
| `assets/` | Versioned example inputs and synthetic measurement fixtures |
| `schemas/` | Intended JSON exchange formats |
| `tests/` | Unit, integration, property, convergence, and verification tests |

## Dependency Rule

Dependencies point inward. The model library and Level 1-3 core know nothing
about HTTP, React, CSV, JSON files, or browser state. The server owns threads;
the experiment runner receives progress through a callback and remains usable by
the CLI and native tests, where it owns no threads. The dashboard renders native
results rather than calculating model metrics.

Keep a new concern in the lowest layer that can own it. A physics decision
belongs in the model library or solver, serialization in `artifact_io`, request
translation in the server, and rendering-only behavior in `web/`. The separate
CFD target may depend on the core and model library but must not become a hidden
dependency of the default simulation pipeline.

For the component diagram and runtime flow, see [architecture.md](architecture.md).

## Build Variants

The standard scripts build Release targets in `build/`:

```bash
./scripts/build.sh
./scripts/test.sh
./scripts/demo.sh
```

Use an isolated Debug build when diagnosing a native failure:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j4
ctest --test-dir build-debug --output-on-failure
```

The project exposes `ESPRESSOLAB_WARNINGS_AS_ERRORS` for release-quality native
builds:

```bash
cmake -S . -B build-warnings -DESPRESSOLAB_WARNINGS_AS_ERRORS=ON
cmake --build build-warnings -j4
```

This configuration is currently expected to fail on a shadowing diagnostic in
`engine/espresso_core/simulator.cpp`. Treat that as an open quality gap, not as
a passing gate, until the warning is fixed.

The web project has independent static and production-build checks:

```bash
npm --prefix web run typecheck
npm --prefix web run build
```

## Test Selection

`./scripts/test.sh` executes the full native suite. Run an individual Catch2
tag or list available tests with:

```bash
./build/tests/espressolab_tests "[cfd]"
./build/tests/espressolab_tests --list-tests
```

Native tests cover unit behavior, whole shots, invariants, axial cells,
calibration, sweeps, and CFD verification. The web project currently has
typechecking and build checks but no browser interaction test harness. Test a
dashboard interaction manually through `./scripts/dev.sh` whenever changing
dragging, selection, chart synchronization, downloads, or accessibility.

Read [testing.md](testing.md) for the intent and limitations of every test
layer. Passing numerical verification tests does not establish that the model
matches real espresso; real-shot validation remains a separate evidence task.

## Changing a Data Contract

Recipe, coefficient, result, sweep, and reference data cross several layers.
Make those edits atomically and do not treat a schema edit as sufficient.

1. Define the field, units, ownership, default behavior, and version impact in
   the C++ domain type.
2. Update the relevant loader, serializer, hash or manifest behavior, and
   runtime validation.
3. Update the JSON schema and the REST documentation in the same change.
4. Update TypeScript types and every UI path that consumes or edits the field.
5. Add loader and serializer tests plus an API or UI test when the field crosses
   a process boundary.
6. Update [data-contracts.md](data-contracts.md) when ownership, units, or
   compatibility semantics change.

The loader and serializer are the current executable contract. Schema files are
an intended external contract, but the application does not automatically run a
JSON Schema validator on every request. Keep them synchronized deliberately.

## Pull-Request Checklist

- Keep dependency direction intact and avoid putting UI or HTTP decisions in
  the solver.
- Preserve deterministic ordering, canonical serialization, and result hashes
  unless a versioned compatibility change is intended.
- Validate finite numeric inputs before they reach a numerical loop.
- Add a regression test for every fixed defect, especially a malformed input or
  invariant violation.
- Run the smallest relevant native, web, and documentation checks before review.
- State whether a change is verified by a test, only manually exercised, or not
  yet validated against real measurements.
