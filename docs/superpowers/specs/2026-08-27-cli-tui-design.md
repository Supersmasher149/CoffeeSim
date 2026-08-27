# CLI TUI Design

## Goal

Add an interactive terminal interface to EspressoLab without changing the
existing file-oriented CLI commands or the native solver's authoritative
calculations. Users launch it with `espressolab_cli tui`.

The first release covers `simulate`, `sweep`, `calibrate`, `synthesize`,
`bench`, `cfd`, `cfd3d`, `params`, `fit-params`, and `version`. It targets
interactive POSIX terminals on macOS and Linux.

## Architecture

The TUI is an application-layer frontend. It calls native loaders, runners,
solvers, calibration APIs, and artifact writers directly; it neither shells out
to the CLI nor depends on the REST server. Existing command handlers are
refactored around shared workflow functions so legacy invocations and the TUI
share input validation, result assembly, units, errors, and hashes.

Terminal rendering and input handling live under `apps/espressolab_cli/tui/`.
The terminal library is isolated behind the TUI target and does not enter the
solver or model-library dependency graph.

Long-running work runs on one worker owned by the TUI. Native execution remains
synchronous and thread-agnostic. A non-serialized execution-control callback
provides cooperative cancellation and coarse status reporting. These controls
are not part of recipe, coefficient, configuration, or result hashes.

Cancellation is checked at safe workflow boundaries. A cancelled single shot,
CFD run, calibration, synthesis, or benchmark does not write incomplete output.
A cancelled sweep retains and exports the runs completed before cancellation,
matching the existing sweep contract.

## Interaction

The home screen groups commands into Run, Calibration/Data, Diagnostics, and
Info. Forms expose the existing command flags through typed fields: paths,
numbers, enums, optional outputs, fit-parameter selection, and holdout IDs.
There is one active job at a time.

The execution screen shows the command, input summary, elapsed time, status,
recent messages, and cancellation control. Sweeps display completed/total;
other workflows display an indeterminate or coarse status because their native
solvers do not expose per-step progress in this release.

Result screens render native metrics, warnings, diagnostics, termination state,
hashes, and artifact paths using the existing unit conversions. Sweep results
include a scrollable run table. The 2D CFD view can display the selected scalar
field. The 3D CFD view displays mesh, snapshot, verification, and artifact
summaries rather than adding a new field contract.

Input and loader failures retain the stable CLI error code, path, message, and
validation issues. The TUI explicitly rejects non-interactive stdin/stdout and
restores terminal state on success, failure, cancellation, resize, and Ctrl-C.

## Dependency and Build

FTXUI `v7.0.3` is vendored as its amalgamated header/source bundle under
`third_party/ftxui/`. CMake builds it as a private target for the CLI. The
vendor refresh script and dependency table record its version, license, and
source. A clean clone remains buildable offline.

## Verification

Pure TUI state transitions, form validation, workflow status, error mapping,
and result presentation are unit-testable without a terminal. Native tests
cover cancellation checkpoints, deterministic workflow equivalence, and
single-run versus partial-sweep artifact behavior. A POSIX PTY smoke matrix
covers launch, representative commands, resize, Ctrl-C, terminal restoration,
and non-TTY failure.

The full native suite, CLI demo, offline build, and web checks remain unchanged.
No recipe/result schema, standard artifact, or result-hash changes are intended.
