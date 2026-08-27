# EspressoLab: Current State and Gaps

**Assessment date:** 2026-08-26
**Scope:** Repository state under `espressolab/`
**Audience:** Project stakeholders, technical contributors, and portfolio reviewers

## Executive Summary

EspressoLab has reached a substantially complete engineering MVP. It is a local
espresso-simulation workbench with a deterministic C++20 simulation core, CLI,
local REST server, React/TypeScript dashboard, reproducible JSON and CSV
artifacts, parameter sweeps, calibration tooling, and a separate 2D CFD solver.

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
| CFD | Separate 2D axisymmetric finite-volume solver with pressure, saturation, enthalpy, and solute transport | Implemented as a separate CLI entry point; verified, not validated |
| CLI | `simulate`, `sweep`, `calibrate`, `synthesize`, `cfd`, `bench`, `params`, `fit-params`, and `version` commands | Implemented |
| REST server | Local API for health, recipes, shots, asynchronous sweeps, cancellation, status, and CSV artifacts | Implemented |
| Dashboard | Recipe controls, draggable pressure and temperature profiles, synchronized charts, comparisons, sweep heat maps, progress/cancellation, exports, diagnostics, and puck cross-section replay | Implemented; some literal browser interactions remain unverified |
| Artifacts | Versioned recipe, coefficient, summary, manifest, JSON/CSV output, and SHA-256 result hashes | Implemented |
| Calibration | Bounded deterministic fitting, held-out validation, leave-one-shot-out workflow, provenance, and synthetic-data workflow | Tooling implemented; real-data evidence absent |
| Schemas | JSON Schema for recipes, coefficients, and shot results | Present |

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

The Level 4 CFD solver adds radial structure and resolves pressure and transport
on a 2D axisymmetric mesh. It remains intentionally separate from the default
pipeline, REST API, dashboard, artifacts, and result hashes.

The model does not claim to resolve pore-scale flow, pump or group-head dynamics,
crema, degassing, flavor, grinder dial settings, or dynamic channel formation.
TDS and extraction yield are engineering outputs, not taste predictions.

### Verification Evidence

The repository documents the following local checks:

- `./scripts/test.sh` passes 115 test cases and 17,385 assertions.
- Native build, dashboard production build, and the CLI demo pass locally.
- The demo covers a baseline shot, a nine-run grind sweep, JSON/CSV artifacts,
  and repeated-run hash equality.
- The baseline reaches 36 g in approximately 29.03 seconds with no clamps and
  near-machine-precision water and solids residuals.
- A 60-second, 100 Hz benchmark records approximately 0.468 ms median runtime,
  substantially inside the documented 20 ms budget.
- Verification tests cover units, correlations, whole-shot behavior, generated
  inputs, invariants, convergence, sweeps, parallel regions, axial cells, CFD,
  calibration recovery, and deterministic leave-one-out mechanics.

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
| Hosted CI evidence has not been confirmed | Clean-clone macOS/Linux claims are not yet backed by observed hosted runs | First hosted workflow runs pass native tests, dashboard build, and demo on both platforms |
| Warnings-as-errors native build fails | A shadowing diagnostic prevents treating compiler warnings as a release gate | Fix the diagnostic and record a clean `ESPRESSOLAB_WARNINGS_AS_ERRORS=ON` build |
| CFD input and convergence hardening is incomplete | Non-finite time steps, saturation overshoot, and non-converged pressure solves need explicit rejection before CFD output can be relied upon operationally | Reject invalid controls, fail or adapt unstable steps, and add regression tests |
| Full browser interaction pass is outstanding | API and served-page checks pass, but profile editing, pinned comparison overlays, synchronized chart behavior, and UI downloads have not all been exercised literally in a browser | A repeatable browser checklist is run and recorded |
| Dashboard has a non-blocking 571.82 kB minified JavaScript chunk warning | Does not block the MVP, but indicates a performance/packaging follow-up for a polished release | Chunk is reduced or the warning is explicitly accepted with measured load impact |
| Portfolio packaging is unfinished | The project is not yet represented by a final demo video and evidence-based resume/project claims | Demo recording and portfolio copy use only verified benchmark and validation results |

### Product and Operational Gaps

- The dashboard has no calibration workflow. The CLI workflow is complete, but a
  dashboard view was deliberately deferred because no real dataset currently
  exists to drive it.
- Runs and sweeps are retained only in process memory. Restarting the server
  loses them, and older entries are evicted from bounded session-local caches.
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
3. Confirm the first hosted CI runs and perform the browser interaction check.
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
