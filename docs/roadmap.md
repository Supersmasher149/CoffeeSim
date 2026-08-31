# Roadmap and Status

This is an evidence-based status report against the August 2026 technical
implementation guide. "Complete" means the capability is present in the
repository and was verified locally; it does not imply that the model has been
validated against real espresso shots.

## Current Position

The engineering MVP is substantially complete. The deterministic C++20 core,
CLI, REST server, React/TypeScript dashboard, artifacts, sweeps, calibration
machinery, and documentation are implemented. The remaining release work is
real-world validation and portfolio packaging, plus hosted CI evidence for the
latest hardening commits.

| Week | Focus from the guide | Status | Evidence |
| --- | --- | --- | --- |
| 1 | Skeleton, domain types, profiles, units, water properties, flow-only CLI | Complete | Core/model targets, unit tests, and CLI are present. |
| 2 | Thermal state, wetting, extraction, mass balances, artifacts, determinism | Complete | Integration, invariant, convergence, and artifact tests pass. |
| 3 | Sweep runner, REST server, React controls, synchronized charts, exports | Complete | Server routes, dashboard source, background sweeps, and production web build are present. |
| 4 | Calibration, edge cases, CI, README, profiling, demo, portfolio packaging | Partial | Calibration and measured-shot comparison tooling, edge-case tests, documentation, benchmark, CLI demo, and hosted macOS/Linux/dashboard evidence at `736cef3` are complete; real calibration, current-head hosted evidence, demo video, and measured portfolio claims remain. |

## Verified Locally

- `./scripts/test.sh` passes: 226 Catch2 test cases and 86,773 assertions.
- `npm --prefix web run typecheck`, `npm --prefix web run test:coverage`, and
  `npm --prefix web run build` succeed. Vite reports a non-blocking 597.30 kB
  minified JavaScript chunk-size warning.
- `npm --prefix web run test:e2e` passes 26 Chromium desktop/mobile tests against
  the built native server.
- The Vitest suite passes 222 tests with its configured coverage thresholds.
- The PTY matrix passes all 15 checks.
- Hosted GitHub Actions macOS, Linux, and dashboard jobs passed at commit
  `736cef3`. Current branch hardening through `94fbe7a` has no equivalent hosted
  run recorded here.
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
| Run and compare shots in the browser | Verified locally | Chromium desktop/mobile Playwright coverage passes for profile editing, keyboard navigation, downloads, determinism, measured-shot comparison, sweeps, and issue #22 recipe binding. |
| Clean-clone macOS and Linux verification | Verified at `736cef3`; current head pending | Hosted macOS, Linux, and dashboard jobs passed at `736cef3`; do not extend that evidence to hardening through `94fbe7a` until another run passes. |

## Completed Week 4 Tooling

- **Calibration engine and CLI.** `espressolab_calibration` loads measured-shot
  data, evaluates the weighted loss from section 11.4, and deterministically
  fits a bounded, named set of physically interpretable coefficients. The CLI
  supports held-out validation, provenance-bearing coefficient files, and JSON
  reports.
- **Synthetic calibration workflow.** `espressolab_cli synthesize` produces
  explicitly flagged synthetic measurement files so the fitting path can be
  exercised without making a scientific claim.
- **Measured-shot comparison.** The local API and dashboard catalogue
  model-ready measured shots and compare a selected shot with one native
  simulation. The response preserves coefficient identity and reports residuals;
  it does not fit coefficients. Current fixtures are synthetic.
- **Explicit CFD3D workflow.** `espressolab_cli cfd3d`, independent case/result
  schemas, `ELF3D-1` artifacts, and asynchronous REST status/snapshot routes are
  implemented separately from standard shot artifacts.
- **Experiment tooling.** The dashboard provides two-dimensional heat maps,
  background sweep jobs with progress and cancellation, and draggable pressure
  and temperature profiles backed by numeric point lists.
- **Grind and flavour tooling.** The separate grinder generates recipe-ready
  particle distributions, and optional bean profiles add a non-authoritative
  sensory overlay without changing physical outputs or beanless hashes.
- **Documentation and profiling.** The README, model, architecture, API,
  testing, and calibration documentation are present; the benchmark command is
  implemented.

## Outstanding Work

- **Fit and validate with real shots.** No real measured shot has been fitted,
  so `default-v1.json` remains an uncalibrated placeholder. The synthetic
  recovery tests validate the fitter, not the espresso model. The CLI now
  supports leave-one-shot-out validation for three or more fixed-setup shots;
  supplying and running those real measurements is the highest-value next step.
- **Confirm current-head hosted CI.** The committed workflow passed on macOS,
  Linux, and the dashboard at `736cef3`. Run it again for hardening through
  `94fbe7a`; the older result is not evidence for those later commits.
- **Finish portfolio packaging.** Record the demo video and write resume bullets
  from the verified benchmark and, after calibration, measured validation data.
- **Calibration dashboard view.** The dashboard now compares measured telemetry
  against fixed coefficients, but fitting remains a CLI workflow. Do not call
  comparison calibration.

## Scope-Cut Order

The guide's scope cuts have mostly been restored: two-dimensional sweeps,
graphical profile editing, and background sweep jobs are implemented. The only
deferred feature from that list is browser-based coefficient fitting. Never cut
deterministic artifacts, tests, warnings, or the Level 1-3 flow/extraction core.

## Fidelity Level 2

Implemented. The puck is one to eight lateral regions in hydraulic parallel:
they share the imposed pressure and inlet-temperature profiles but carry their
own wetting, flow, lumped thermal, and extraction state, and unequal
permeability multipliers stand in for a fixed, first-order channel. A recipe
without `parallel_regions` resolves to one uniform region and reproduces the
Level 1 numbers, which `tests/integration/test_shots.cpp` asserts.

Aggregate samples, summary, diagnostics and the CSV export are unchanged, so
the dashboard contract held across the change; each run additionally reports a
final `regions` array. `assets/recipes/channelled.json` is the worked example.
See [model.md](model.md) for the equations and `2026-08-25-level-2-parallel-regions-design.md`
for the design.

What Level 2 deliberately does not do: no lateral exchange between regions, no
dynamic channel formation, no axial structure, and no dashboard region editor.

## Fidelity Level 3

Implemented. Each lateral region divides into 1 to 32 stacked axial
finite-volume cells in hydraulic series, so the puck resolves a wetting front, a
temperature profile, and a solute concentration gradient from the screen to the
basket. `axial_cells` defaults to 1 and reproduces the Level 2 shot exactly.
`assets/recipes/axial-resolved.json` is the worked example. See
[model.md](model.md) and `2026-08-26-level-3-axial-cells-design.md`.

Refining the grid raises the reported yield, because a resolved column lets the
upper cells extract into fresh water while the lower cells work against liquid
that is already loaded. The baseline moves from 18.18 % at one cell to 22.28 %
at sixteen, with the gap between successive doublings roughly halving.

## Fidelity Level 4: the CFD solver

Implemented as separate `CfdSolver` and `Cfd3dSolver` paths, reached through
`espressolab_cli cfd` and `espressolab_cli cfd3d`. Neither is wired into the
default Level 1-3 shot pipeline or its hashes. CFD3D has separate case/result
schemas, file artifacts, and asynchronous REST status/snapshot routes; the 2D
solver remains CLI-only.

It solves `div(lambda_t grad p) = 0` on a 2D axisymmetric (r, z) finite-volume
mesh by red-black SOR, then advances water saturation by IMPES fractional flow
with donor-upwinded enthalpy and solute transport. Face fluxes are limited on
the donor cell, and because the limiter scales the shared face value the
balances close to machine precision. Full equations in [model.md](model.md).

The radial coordinate is what it buys: a channelled recipe at Level 2 or 3 is
told how the flow splits, whereas the CFD solver is told only where the
permeability differs and resolves the radial pressure gradient itself.

### Verified, not validated

This distinction is the important one, so it is worth stating plainly. The
`[cfd][verification]` tests check the solver against the equations it claims to
solve: discrete divergence ~1e-8 1/s, mass residuals ~1e-17 kg, axisymmetry of a
uniform bed below 1e-7, an isothermal pressure field linear in depth to 5e-4
with the departure shrinking under refinement, Darcy velocity within 1 % of
analytic, and monotone mesh convergence.

None of that says the equations describe espresso. The coefficients are the same
uncalibrated set as the rest of the project, so the CFD solver resolves
structure that no measurement has checked. It is a more detailed answer to a
question nobody has yet confirmed is the right question.

### What it is not

The momentum closure is Darcy at the representative-elementary-volume scale,
which is the standard porous-media formulation. The pore geometry is not meshed,
so this is not pore-resolved DNS, there is no turbulence model, and inertia
inside the pores is not resolved. A Forchheimer inertial term is present in the
configuration and defaults to zero.

## Recommended Next Modeling Extension

Real-shot calibration, still, and now more urgently than before. Levels 2, 3 and
4 each resolve more structure than the last, and none of it has been compared
against a measured shot. Additional fidelity is the thing this project least
needs; measurements are the thing it most needs.
