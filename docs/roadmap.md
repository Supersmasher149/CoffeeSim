# Roadmap and Status

This is an evidence-based status report against the August 2026 technical
implementation guide. "Complete" means the capability is present in the
repository and was verified locally; it does not imply that the model has been
validated against real espresso shots.

## Current Position

The engineering MVP is substantially complete. The deterministic C++20 core,
CLI, REST server, React/TypeScript dashboard, artifacts, sweeps, calibration
machinery, and documentation are implemented. The remaining release work is
real-world validation and portfolio packaging, plus CI that proves a clean
cross-platform build.

| Week | Focus from the guide | Status | Evidence |
| --- | --- | --- | --- |
| 1 | Skeleton, domain types, profiles, units, water properties, flow-only CLI | Complete | Core/model targets, unit tests, and CLI are present. |
| 2 | Thermal state, wetting, extraction, mass balances, artifacts, determinism | Complete | Integration, invariant, convergence, and artifact tests pass. |
| 3 | Sweep runner, REST server, React controls, synchronized charts, exports | Complete | Server routes, dashboard source, background sweeps, and production web build are present. |
| 4 | Calibration, edge cases, CI, README, profiling, demo, portfolio packaging | Partial | Calibration tooling, edge-case tests, documentation, benchmark, CLI demo, and a macOS/Linux CI workflow are complete; real calibration, hosted CI evidence, demo video, and measured portfolio claims remain. |

## Verified Locally

- `./scripts/test.sh` passes: 78 test cases and 14,474 assertions.
- `npm run build` succeeds for the dashboard. Vite reports a non-blocking
  559 kB JavaScript chunk-size warning.
- `./scripts/demo.sh` completes the documented CLI acceptance path:
  baseline simulation, nine-run grind-size sweep, JSON/CSV artifacts, and an
  identical SHA-256 result hash on rerun.
- The Vite-served dashboard loads and proxies `/api/v1` to the local server.
  Through that path, shot execution, shot CSV export, completed sweep CSV export,
  and cancelled-sweep partial CSV export all succeed.
- `espressolab_cli bench` records a 60-second, 100 Hz shot in 0.468 ms median,
  42.7x inside the guide's 20 ms performance budget.
- The baseline run reaches the 36 g target in 29.03 s with no clamps and closes
  water and solids residuals near machine precision.

## Guide Acceptance Gate

| Gate | Status | Notes |
| --- | --- | --- |
| Build native core and dashboard | Verified locally | CMake build and production web build succeed. |
| Run the baseline recipe | Verified locally | CLI demo reaches its beverage-mass target. |
| Complete a grind-size sweep | Verified locally | The demo exports a nine-run sweep. |
| Export JSON and CSV artifacts | Verified locally | The demo writes shot and sweep artifacts. |
| Reproduce the same result hash | Verified locally | The demo reports matching SHA-256 hashes. |
| Pass conservation and convergence tests | Verified locally | Covered by the passing test suite. |
| Run and compare shots in the browser | Partially verified | The Vite page, API proxy, shot flow, completed sweep, cancellation, and exports pass; literal browser interactions, including profile editing and pinned comparison overlays, remain unverified. |
| Clean-clone macOS and Linux verification | CI configured; hosted runs outstanding | GitHub Actions runs the native suite, dashboard build, and CLI demo on both platforms; this local review cannot confirm the first hosted run. |

## Completed Week 4 Tooling

- **Calibration engine and CLI.** `espressolab_calibration` loads measured-shot
  data, evaluates the weighted loss from section 11.4, and deterministically
  fits a bounded, named set of physically interpretable coefficients. The CLI
  supports held-out validation, provenance-bearing coefficient files, and JSON
  reports.
- **Synthetic calibration workflow.** `espressolab_cli synthesize` produces
  explicitly flagged synthetic measurement files so the fitting path can be
  exercised without making a scientific claim.
- **Experiment tooling.** The dashboard provides two-dimensional heat maps,
  background sweep jobs with progress and cancellation, and draggable pressure
  and temperature profiles backed by numeric point lists.
- **Documentation and profiling.** The README, model, architecture, API,
  testing, and calibration documentation are present; the benchmark command is
  implemented.

## Outstanding Work

- **Fit and validate with real shots.** No real measured shot has been fitted,
  so `default-v1.json` remains an uncalibrated placeholder. The synthetic
  recovery tests validate the fitter, not the espresso model. The CLI now
  supports leave-one-shot-out validation for three or more fixed-setup shots;
  supplying and running those real measurements is the highest-value next step.
- **Confirm hosted CI runs.** The committed macOS/Linux GitHub Actions workflow builds
  the native targets, runs `./scripts/test.sh`, builds the dashboard, and runs
  `./scripts/demo.sh` from a fresh checkout. Confirm the first hosted runs pass
  after this change is pushed.
- **Run a browser interaction check.** The served page and API workflow are
  verified. Use a controllable browser to edit a recipe and profile, pin runs for
  comparison, inspect synchronized charts, and confirm downloads from the UI.
- **Finish portfolio packaging.** Record the demo video and write resume bullets
  from the verified benchmark and, after calibration, measured validation data.
- **Calibration dashboard view.** The CLI workflow is complete. A dashboard
  workflow remains deferred because it has no real data to drive it and was
  explicitly low priority in the guide's scope-cut order.

## Scope-Cut Order

The guide's scope cuts have mostly been restored: two-dimensional sweeps,
graphical profile editing, and background sweep jobs are implemented. The only
deferred feature from that list is the calibration dashboard view. Never cut
deterministic artifacts, tests, warnings, or the uniform-puck flow/extraction
core.

## Recommended Next Modeling Extension

After real-shot calibration, add parallel puck regions with different
permeability (fidelity level 2). Channelling is the largest known limitation of
the current uniform-puck model, and parallel flow regions extend the existing
flow calculation more directly than axial thermal cells.
