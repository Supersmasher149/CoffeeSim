# EspressoLab: Current State and Gaps

**Assessment date:** 2026-08-27
**Scope:** Repository state under `espressolab/`
**Audience:** Project stakeholders, technical contributors, and portfolio reviewers

## Executive Summary

EspressoLab has reached a substantially complete engineering MVP. It is a local
espresso-simulation workbench with a deterministic C++20 simulation core, CLI,
local REST server, React/TypeScript dashboard, reproducible JSON and CSV
artifacts, parameter sweeps, measured-shot comparison, calibration tooling, and
separate 2D axisymmetric and Cartesian 3D CFD solvers.

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

The project is intentionally a local engineering workbench. It has no accounts,
authentication, cloud deployment, database, or remote backend service; its REST
server is local and session-bound.

---

## Current State

### Implemented Product Surface

| Area | Current capability | Status |
| --- | --- | --- |
| Simulation core | Deterministic C++20 shot solver with pressure and inlet-temperature profiles, wetting, thermal state, extraction, transport, termination, warnings, and mass-balance diagnostics | Implemented and locally tested |
| Model fidelity | Level 1 lumped puck behavior, Level 2 lateral parallel regions, and Level 3 stacked axial finite-volume cells | Implemented |
| CFD | Separate 2D axisymmetric and Cartesian 3D finite-volume solvers with pressure, saturation, enthalpy, and solute transport | Implemented as explicit CLI paths; 3D also has asynchronous REST status/snapshot routes; verified, not validated |
| CLI | `simulate`, `sweep`, `calibrate`, `synthesize`, `cfd`, `cfd3d`, `bench`, `params`, `fit-params`, and `version` commands | Implemented |
| Terminal UI | `espressolab_cli tui`: guided forms over every CLI command, calling the same shared workflow services as the file-oriented commands; cooperative cancellation; POSIX-only | Implemented; native logic is locally tested, while the current 14-check PTY script was not rerun for this refresh |
| REST server | Local API for health, recipes, references, measured-shot catalogue/comparison, shots, asynchronous sweeps and CFD3D jobs, cancellation, status, snapshots, and CSV artifacts | Implemented |
| Dashboard | Recipe controls, measured-shot comparison, draggable pressure and temperature profiles, synchronized charts, pinned runs, sweep heat maps, progress/cancellation, exports, diagnostics, and puck cross-section replay | Implemented; some literal browser interactions remain unverified |
| Artifacts | Versioned standard-shot and CFD3D inputs/results, JSON/CSV output, `ELF3D-1` snapshot fields, manifests, and SHA-256 hashes | Implemented |
| Calibration | Bounded deterministic fitting, held-out validation, leave-one-shot-out workflow, provenance, and synthetic-data workflow | Tooling implemented; real-data evidence absent |
| Schemas | JSON Schema for recipes, coefficients, shot results, and CFD3D cases/results | Present |

### Architecture

The dependency direction is deliberately inward:

```text
web -> local REST server -> experiment runner -> simulation core
                                      -> artifact I/O
                                             -> model library
```

The physics core does not know about HTTP, React, plotting, or filesystem
locations. The dashboard renders authoritative values returned by the native
solver rather than reimplementing the model in browser code. The server owns
background sweep workers and cancellation, leaving the runner usable by the CLI
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
crema, degassing, flavor, grinder dial settings, or dynamic channel formation.
TDS and extraction yield are engineering outputs, not taste predictions.

### Verification Evidence

The repository documents the following local checks:

- `./scripts/test.sh` passes 165 Catch2 test cases and 18,213 assertions. It does
  not run PTY, dashboard, demo, or warnings-as-errors checks.
- The dashboard production build succeeds and reports the existing non-blocking
  583.67 kB minified JavaScript chunk warning.
- GitHub Actions macOS, Linux, and dashboard jobs passed at commit `736cef3`.
  Later hardening through current branch commit `94fbe7a` has no equivalent
  hosted run recorded here.
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
- A separate POSIX PTY script (`tests/pty/tui_smoke.py`) defines 14 checks for
  launch, resize, guided execution, long-form scrolling, Ctrl-C, terminal
  restoration, and non-TTY rejection. It is outside Catch2/`ctest` and was not
  rerun for this documentation refresh.

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
| Current-branch hosted CI evidence is absent | Commit `736cef3` passed macOS/Linux/dashboard jobs, but later hardening through `94fbe7a` has only local evidence recorded here | Hosted workflow passes at the current branch head |
| Full browser interaction pass is outstanding | API and served-page checks pass, but profile editing, pinned comparison overlays, synchronized chart behavior, and UI downloads have not all been exercised literally in a browser | A repeatable browser checklist is run and recorded |
| Dashboard has a non-blocking 583.67 kB minified JavaScript chunk warning | Does not block the MVP, but indicates a performance/packaging follow-up for a polished release | Chunk is reduced or the warning is explicitly accepted with measured load impact |
| Portfolio packaging is unfinished | The project is not yet represented by a final demo video and evidence-based resume/project claims | Demo recording and portfolio copy use only verified benchmark and validation results |

### Product and Operational Gaps

- The dashboard compares measured telemetry with a fixed coefficient set but
  does not fit coefficients. Calibration remains a CLI workflow; comparison
  must not be presented as fitting or validation.
- Shots, sweeps, and CFD3D jobs are retained only in process memory. Restarting
  the server loses them, and older entries are evicted from bounded
  session-local caches.
- The API is local-only and has no authentication, authorization, CORS support,
  cloud deployment, persistence, or multi-user operation. These are out of MVP
  scope, but they are gaps for any hosted or collaborative product.
- There is no documented installer or single packaged distribution for the
  native server, CLI, and dashboard. Users currently build from the repository
  and run the supplied scripts.
- The frontend manifest exposes build and typecheck scripts, but the repository
  does not document a dedicated automated frontend interaction test suite. UI
  confidence currently comes from the local served workflow and source-level
  implementation plus native/API tests.

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
- Extraction and TDS do not predict flavor or sensory quality.
- Additional model fidelity is not a substitute for measurements. Levels 2, 3,
  and 4 currently resolve more structure than has been checked against real
  data.

## Recommended Sequence

1. Collect and commit a small real-shot dataset from one fixed setup, following
   `docs/calibration.md` and `assets/measured_shots/README.md`.
2. Run leave-one-out calibration, inspect held-out mass/time/TDS errors, and
   publish a new provenance-bearing coefficient file only if the acceptance gate
   passes.
3. Run hosted CI at the current branch head and perform the browser interaction
   check.
4. Update the README and portfolio materials with measured claims only; record
   the demo video.
5. Decide whether the project remains a local engineering workbench or needs
   persistence, packaging, and deployment work. Do not add those capabilities
   before the validation decision unless the product goal changes.
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
