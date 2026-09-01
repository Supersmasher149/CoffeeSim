# EspressoLab: Current State and Gaps

**Assessment date:** 2026-09-01
**Scope:** Repository state under `espressolab/`
**Audience:** Project stakeholders, technical contributors, and portfolio reviewers

## Executive Summary

EspressoLab has reached a substantially complete engineering MVP. It is a
local-first espresso-simulation workbench with a deterministic C++20 simulation
core, CLI, REST server, React/TypeScript dashboard, reproducible JSON and CSV
artifacts, parameter sweeps, measured-shot comparison, calibration tooling, and
separate 2D axisymmetric and Cartesian 3D CFD solvers. The repository also has
an optional single-container Fly.io deployment for the dashboard and REST server.

The project is strong on software structure, reproducible experiments, and
automated verification of its standard configurations. It is not yet a
scientifically validated espresso model or a production product. The
highest-value remaining work is to collect real, consistently documented shots,
fit and evaluate the model against held-out measurements, close the numerical
hardening and quality-gate gaps, and package the result for presentation.

The most important status distinction is:

> The implementation and its numerical machinery are verified locally, but the
> model coefficients and espresso behavior are not validated against real shots.

## Project Purpose

EspressoLab simulates a shot from controllable brew inputs and exposes pressure,
temperature, flow, beverage mass, TDS, and extraction over time. It supports
reproducible recipe comparison through parameter sweeps.

The project remains local-first: the default development workflow runs the REST
server and dashboard on the user's machine. The repository also includes a
single-container Fly.io deployment at
https://espressolab-dashboard.fly.dev/. The hosted instance has no accounts,
authentication, database, or durable storage; its server state is session-bound.

---

## Current State

### Implemented Product Surface

| Area | Current capability | Status |
| --- | --- | --- |
| Simulation core | Deterministic C++20 shot solver with pressure and inlet-temperature profiles, wetting, thermal state, extraction, transport, termination, warnings, and mass-balance diagnostics | Implemented and locally tested |
| Model fidelity | Level 1 lumped puck behavior, Level 2 lateral parallel regions, and Level 3 stacked axial finite-volume cells | Implemented |
| CFD | Separate 2D axisymmetric and Cartesian 3D finite-volume solvers with pressure, saturation, enthalpy, and solute transport | Implemented as explicit CLI paths; 3D also has asynchronous REST status/snapshot routes; verified, not validated |
| CLI | `simulate`, `sweep`, `calibrate`, `synthesize`, `cfd`, `cfd3d`, `grind`, `bench`, `params`, `fit-params`, and `version` commands | Implemented |
| Terminal UI | `espressolab_cli tui`: guided forms over every CLI command, calling the same shared workflow services as the file-oriented commands; cooperative cancellation; POSIX-only | Implemented; native logic and the 15-check PTY matrix pass locally |
| REST server | API for health, recipes, references, measured-shot catalogue/comparison, shots, asynchronous sweeps and CFD3D jobs, cancellation, status, snapshots, and CSV artifacts | Implemented locally and packaged with the dashboard for Fly.io; session-bound and unauthenticated |
| Dashboard | Recipe controls, measured-shot comparison, draggable pressure and temperature profiles, synchronized charts, pinned runs, sweep heat maps, progress/cancellation, exports, diagnostics, and puck cross-section replay | Implemented; 222 Vitest tests and 26 Chromium desktop/mobile Playwright tests pass locally |
| Artifacts | Versioned standard-shot and CFD3D inputs/results, JSON/CSV output, `ELF3D-1` snapshot fields, manifests, and SHA-256 hashes | Implemented |
| Calibration | Bounded deterministic fitting, held-out validation, leave-one-shot-out workflow, provenance, and synthetic-data workflow | Tooling implemented; real-data evidence absent |
| Schemas | JSON Schema for recipes, coefficients, shot results, CFD3D cases/results, grinder specs/results, and bean profiles | Present |

### Architecture

The dependency direction is deliberately inward:

```text
web -> local REST server -> experiment runner -> simulation core
                                      -> artifact I/O
                                             -> model library
```

The physics core does not know about HTTP, React, plotting, or filesystem
locations. The dashboard renders authoritative values returned by the native
solver rather than reimplementing the model in browser code. Application layers
own background workers and cancellation, leaving the runner usable by the CLI
and tests without threads.

### Model and Scientific Scope

The default Level 1-3 pipeline models Darcy flow through a porous puck,
pressure-dependent compression and porosity, wetting and pore filling, thermal
exchange, extraction kinetics, dissolved-solids transport, and aggregate
metrics. Level 2 represents fixed lateral permeability differences as parallel
regions. Level 3 resolves a wetting front and concentration gradient through
stacked axial cells.

The Level 4 CFD solvers add spatial structure: 2D axisymmetric `(r,z)` and
Cartesian 3D `(x,y,z)`. Both remain separate from the default shot pipeline and
its result hashes. The 2D path is CLI-only; the 3D path has its own CLI,
artifacts, schemas, asynchronous REST jobs, and snapshot responses.

The model does not claim to resolve pore-scale flow, pump or group-head dynamics,
crema, degassing, grinder dial settings, or dynamic channel formation.
TDS and extraction yield are engineering outputs, not taste predictions. The
optional sensory overlay is a heuristic layered on top of them from authored
priors, not a taste measurement.

### Verification Evidence

The repository documents the following local checks:

- `./scripts/test.sh` passes 226 Catch2 test cases and 86,773 assertions. It does
  not run PTY, dashboard, demo, or warnings-as-errors checks.
- The dashboard typecheck, coverage suite, and production build succeed. PR #52
  reduced the initial JavaScript payload from 597,368 B / 172,158 B gzip to
  190,915 B / 61,255 B gzip (68.0% raw / 64.4% gzip). PR #52 brought every
  emitted JavaScript chunk below Vite's 500 kB threshold. Total emitted
  JavaScript is essentially unchanged, changing from 597,368 B / 172,158 B
  gzip to 595,450 B / 172,493 B gzip. This is an initial-load improvement, not
  a reduction in total transfer.
- GitHub Actions macOS, Linux, dashboard, and full nightly browser-matrix jobs
  passed for current `origin/main` commit `347177c` in
  [run 33508004359](https://github.com/Supersmasher149/CoffeeSim/actions/runs/33508004359).
- The demo covers a baseline shot, a nine-run grind sweep, JSON/CSV artifacts,
  and repeated-run hash equality.
- The baseline reaches 36 g in approximately 29.03 seconds with no clamps and
  near-machine-precision water and solids residuals.
- A 60-second, 100 Hz benchmark records approximately 0.468 ms median runtime,
  substantially inside the documented 20 ms budget.
- Verification tests cover units, correlations, whole-shot behavior, generated
  inputs, invariants, convergence, sweeps, parallel regions, axial cells, CFD,
  calibration recovery, deterministic leave-one-out mechanics, native
  cancellation checkpoints, measured-shot comparison, and the TUI's pure
  (terminal-free) navigation, form, and shared-workflow logic.
- A separate POSIX PTY script (`tests/pty/tui_smoke.py`) defines 15 checks for
  launch, resize, guided execution, long-form scrolling, Ctrl-C, terminal
  restoration, and non-TTY rejection. It is outside Catch2/`ctest` and passes
  locally.
- Playwright covers the real native-server dashboard workflow in Chromium
  desktop and mobile, including profile dragging, keyboard navigation, downloads,
  determinism, measured-shot comparison, sweeps, and issue #22 recipe binding.

These checks establish implementation behavior and numerical consistency. They
do not establish that the equations or coefficients describe real espresso.

## Current Gaps

### Highest Priority: Real-World Validation

No non-synthetic measured shot has been fitted. The files under
`assets/measured_shots/` are explicitly marked synthetic and are generated from
the model, so they can test the calibration machinery but cannot validate the
model.

Required evidence:

- At least three shots from one consistently documented machine setup for
  leave-one-out validation.
- Recorded time and beverage-mass series, final shot time, and setup metadata.
- Measured TDS and pressure where instruments provide them, rather than guessed
  values.
- A new versioned coefficient file with dataset reference, date, and limitations.
- A validation report meeting the documented held-out error thresholds.

Until this is complete, `default-v1.json` must remain described as uncalibrated,
and output should be presented as a model result rather than a prediction.

### Release and Verification Gaps

| Gap | Impact | Completion signal |
| --- | --- | --- |
| Dashboard initial-load packaging | Resolved by PR #52: initial JavaScript fell from 597,368 B / 172,158 B gzip to 190,915 B / 61,255 B gzip, and every emitted JavaScript chunk is below 500 kB. Total emitted JavaScript remains essentially unchanged at 595,450 B / 172,493 B gzip. | Future performance reporting distinguishes initial payload from total transfer |
| Portfolio packaging is unfinished | The project is not yet represented by a final demo video and evidence-based resume/project claims | Demo recording and portfolio copy use only verified benchmark and validation results |

### Product and Operational Gaps

- The dashboard compares measured telemetry with a fixed coefficient set but
  does not fit coefficients. Calibration remains a CLI workflow; comparison
  must not be presented as fitting or validation.
- Shots, sweeps, and CFD3D jobs are retained only in process memory. Restarting
  the server loses them, and older entries are evicted from bounded
  session-local caches.
- The API has no authentication, authorization, CORS support, persistence, or
  multi-user operation. The Fly.io deployment is a single shared instance, not
  a hosted collaborative product, and these remain gaps for that use case.
- There is no documented installer or standalone distribution for the native
  server, CLI, and dashboard. The Dockerfile provides a deployable
  dashboard/server image and `fly.toml` provides the Fly.io configuration, but
  the CLI/TUI still require a source build.
- The frontend browser suite currently covers Chromium desktop and mobile on
  every pull request; Firefox/WebKit remain a nightly matrix. The current
  `origin/main` has hosted evidence for both native platforms and the full
  browser matrix in [run 33508004359](https://github.com/Supersmasher149/CoffeeSim/actions/runs/33508004359).

### Model Limitations That Remain Gaps by Design

These are not defects in the current MVP, but they limit what conclusions the
project can support:

- Default Level 1-3 behavior has no radial structure within a region, lateral
  exchange, or dynamic channel formation.
- The CFD solver is Darcy-scale and representative-elementary-volume based, not
  pore-resolved Navier-Stokes or particle-resolved DNS.
- Pressure and inlet-temperature profiles are inputs. Pump and group-head
  dynamics are not inferred from measurements.
- There is no universal conversion from grinder dial number to particle size.
  A recipe may now carry a measured particle size distribution
  (`puck.grind`), from which the solver derives the representative diameter and
  extracts each size class at its own rate — but the distribution is still a
  physical input in microns, not a grinder setting, and nothing maps a dial to
  one. No distribution in this repository has been compared against a measured
  shot; the size-resolved path is verified against the equations it claims to
  solve, not validated against reality.
- `espressolab_cli grind` models comminution (burr gap to distribution) with a
  standard population balance. It conserves mass to machine epsilon and is
  deterministic, but its coefficients are a plausible baseline rather than a fit,
  and no distribution it produces has been compared against a measured one. It
  sits outside the shot pipeline and has no REST or dashboard surface.
- A wide burr gap can be grinder-valid (`burr_gap_um` within its documented
  50–1500 µm range) yet produce a distribution the recipe schema rejects, even
  when its d32 sits well inside the shot model's supported 150–800 µm band. The
  output grid always ends exactly at `bean_diameter_um`, so at the shipped
  default (6000 µm) gaps above roughly 750–800 µm place a tail bin over the
  recipe's 2000 µm per-bin cap — sometimes carrying a fraction of a percent of
  total mass — and the whole distribution is rejected. Neither raising `bins`
  nor `bean_diameter_um` recovers usability (both push the same or more mass
  further past the cap); only shrinking `bean_diameter_um` toward the schema's
  2000 µm floor does, which is a workaround, not a physically faithful whole
  bean size. See `docs/model.md`'s grinder section. Not yet triaged: whether
  the fix belongs in the grinder model (e.g. dropping negligible-mass tail
  bins before validation) or is accepted as an input-space boundary.
- Extraction and TDS do not predict flavor or sensory quality. The sensory
  overlay is a separate, deliberately non-authoritative layer: its solute-class
  shares, relative rates and axis weights are authored priors, none has been
  fitted, and no predicted axis has been compared with a tasting panel. It is
  excluded from calibration by design. A small descriptive-panel or triangle-test
  dataset would be the first real check on it.
- Additional model fidelity is not a substitute for measurements. Levels 2, 3,
  and 4 currently resolve more structure than has been checked against real
  data.

## Recommended Sequence

1. Collect and commit a small real-shot dataset from one fixed setup, following
   `docs/calibration.md` and `assets/measured_shots/README.md`.
2. Run leave-one-out calibration, inspect held-out mass/time/TDS errors, and
   publish a new provenance-bearing coefficient file only if the acceptance gate
   passes.
3. Keep hosted CI green for future branch heads.
4. Update the README and portfolio materials with measured claims only; record
   the demo video.
5. Decide whether the project needs persistence, multi-user operation, and
   broader packaging beyond the existing single-container Fly.io deployment.
   Do not add those capabilities before the validation decision unless the
   product goal changes.
6. Treat a calibration dashboard and further model fidelity as follow-up work
   after real-shot evidence exists.

## Status Definitions

- **Implemented:** The capability is present in the repository.
- **Verified locally:** The documented local build, test, or workflow has passed.
- **Partially verified:** Some paths are exercised, but a stated acceptance check
  remains incomplete.
- **Verified, not validated:** The implementation is checked against equations,
  invariants, or synthetic fixtures, but not against real espresso measurements.
- **Gap:** Work or evidence still required before the corresponding claim can be
  made confidently.

## Evidence Map

- Product overview and limitations: [`README.md`](../README.md)
- Current implementation status: [`docs/roadmap.md`](roadmap.md)
- Component boundaries and data flow: [`docs/architecture.md`](architecture.md)
- REST contract: [`docs/api.md`](api.md)
- Equations and model assumptions: [`docs/model.md`](model.md)
- Calibration workflow and acceptance criteria: [`docs/calibration.md`](calibration.md)
- Test layers and verification limits: [`docs/testing.md`](testing.md)
- Measured-shot data requirements: [`assets/measured_shots/README.md`](../assets/measured_shots/README.md)
