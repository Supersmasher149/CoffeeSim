# Development Guide

This guide defines how to work on EspressoLab without breaking its dependency
boundaries, reproducibility guarantees, or cross-language data contracts.

## Repository Map

| Location | Responsibility |
| --- | --- |
| `engine/espresso_core/` | Domain types, validation, profiles, state stepping, termination, and invariants for the Level 1-3 solver |
| `engine/model_library/` | Water properties, puck geometry and permeability, heat, and extraction correlations |
| `engine/cfd/` | Separate Level 4 axisymmetric porous-media solver |
| `engine/cfd3d/` | Separate Level 4b Cartesian 3D porous-media solver and field snapshots |
| `engine/grind/` | Separate comminution model (burr geometry to particle size distribution) with its own IO |
| `engine/artifact_io/` | JSON/CSV load and dump, canonical hashes, manifests, and artifact files |
| `engine/experiment_runner/` | Sweep axes, Cartesian execution, progress, and aggregate export (single-threaded) |
| `engine/calibration/` | Measured-shot loading, loss functions, fitting, and validation reports |
| `engine/reference_io/` | Read-only reference-shot catalogue loading |
| `include/espressolab/` | Public C++ headers shared across targets, including the cancellation/status contract (`execution.hpp`) and the bounded queue behind parallel sweeps (`ring_buffer.hpp`) |
| `apps/espressolab_cli/` | Argv parsing and `workflows.{hpp,cpp}`: shared CLI workflow services (load, validate, run, write artifacts) used by both the legacy commands and the TUI; `sweep_batch_runner.{hpp,cpp}` owns the parallel sweep path |
| `apps/espressolab_cli/tui/` | The interactive terminal UI: `tui_forms.{hpp,cpp}` (navigation/forms, no terminal dependency) and `tui.cpp` (FTXUI rendering and the worker thread) |
| `apps/espressolab_server/` | Local REST translation, in-memory runs, and background sweep jobs |
| `web/src/` | React controls and visualizations for server-provided results |
| `assets/` | Versioned example inputs and synthetic measurement fixtures |
| `schemas/` | Intended JSON exchange formats |
| `tests/` | Unit, integration, property, convergence, and verification tests |
| `tests/pty/` | POSIX PTY smoke matrix for the TUI, run separately from `ctest` |
| `tests/server/`, `tests/cli/` | Black-box smoke scripts that start a real built binary; registered under `ctest -L server`, not in `test.sh` |
| `tests/schemas/` | Schema-vs-loader contract check; needs `jsonschema`, run by hand |

## Dependency Rule

Dependencies point inward. The model library and Level 1-3 core know nothing
about HTTP, React, CSV, JSON files, or browser state. Threads are owned by the
applications, never the engine: the experiment runner receives progress through
a callback and remains usable by the CLI and native tests, where it owns no
threads. The dashboard renders native results rather than calculating model
metrics.

The TUI follows the same rule from the other direction: it is an
application-layer frontend that calls native loaders, solvers, calibration
APIs, and artifact writers directly through `workflows.{hpp,cpp}` -- it does
not shell out to the CLI and does not depend on the REST server. FTXUI is
linked only by the `espressolab_cli` executable; `espressolab_cli_support`
(the shared workflow services and the TUI's pure navigation/form logic) has no
terminal UI dependency at all, which is what makes it usable from
`espressolab_tests` without a TTY. Native execution stays thread-agnostic:
cooperative cancellation and coarse status are a callback pair
(`espressolab::CancellationCallback` in `execution.hpp`), checked at solver,
pressure-iteration, calibration, and sweep boundaries, not a thread the solver
owns. Exactly three thread owners exist -- the server's job threads, the TUI's
worker thread (in `tui.cpp`), and the parallel sweep pool in
`sweep_batch_runner.cpp` -- and all three call the same synchronous,
thread-agnostic native APIs. If you add a fourth, it belongs in an application,
not in `engine/`.

`sweep_batch_runner.cpp` deserves its own note, because it is the one place
where two execution paths must agree. It runs sweep points concurrently while
`engine/experiment_runner/` stays sequential, and both call the same pure
helpers from `experiment.hpp`, so a spec run with `--workers 8` must yield the
same runs, in the same order, with the same per-run result hashes as the
sequential path. Changing either path without the other is a determinism bug;
`tests/integration/test_sweep_batch_runner.cpp` is what catches it.

Keep a new concern in the lowest layer that can own it. A physics decision
belongs in the model library or solver, serialization in `artifact_io`, request
translation in the server, and rendering-only behavior in `web/` or `tui.cpp`.
The separate CFD targets may depend on the core and model library, and the
grind target on the core types, but none of them may become a hidden dependency
of the default simulation pipeline or change its artifacts or hashes.

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

This configuration passes as of the TUI work (#23): the shadowing diagnostic
this note used to describe was resolved by the `simulator.cpp` puck-region
refactor.

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
calibration, sweeps (sequential and parallel), CFD verification, the particle
size distribution and grinder models (`[grind]`, `[grind_sim]`), cancellation
checkpoints (`[cancellation]`), and the TUI's pure navigation/form/workflow
logic (`[tui]`, `[cli_workflows]`)
without a terminal. The web project currently has typechecking and build
checks but no browser interaction test harness. Test a dashboard interaction
manually through `./scripts/dev.sh` whenever changing dragging, selection,
chart synchronization, downloads, or accessibility.

The TUI's terminal rendering, input handling, and resize/Ctrl-C behavior are
not exercised by `ctest` -- they need a real pseudo-terminal, not just the
Catch2 binary. Run the separate PTY smoke matrix on a POSIX machine after any
change to `apps/espressolab_cli/tui/`:

```bash
python3 tests/pty/tui_smoke.py
```

It fails with a diagnostic (not a crash) when its prerequisites -- a built
binary, a PTY-capable environment -- are missing.

Read [testing.md](testing.md) for the intent and limitations of every test
layer. Passing numerical verification tests does not establish that the model
matches real espresso; real-shot validation remains a separate evidence task.

## Changing a Data Contract

Recipe, coefficient, result, sweep, measured-shot, CFD3D, grinder, and
reference data cross several layers. Make those edits atomically and do not
treat a schema edit as sufficient.

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
