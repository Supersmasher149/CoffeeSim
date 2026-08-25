# Roadmap and status

## Where this scaffold sits against the four-week plan

| Week | Focus | Status |
| --- | --- | --- |
| 1 | Skeleton, domain types, profiles, units, water properties, flow-only CLI | Complete |
| 2 | Thermal state, wetting, extraction, mass balances, artifacts, determinism | Complete |
| 3 | Sweep runner, REST server, React controls, synchronized charts, exports | Complete |
| 4 | Calibration, edge cases, CI, README, profiling, demo and portfolio packaging | Partial — see below |

Week 4 is where the remaining work is, and most of it needs something this
scaffold cannot manufacture: real measured shots.

## Not done

- **Measured-shot calibration.** No shot has been fitted. The workflow, the file
  layout and the loss function are documented; the data is not there.
  This is the single biggest gap between "runs" and "means something".
- **Two-dimensional heat maps in the dashboard.** The 2D sweep runs and exports
  correctly (`assets/sweeps/temperature-x-grind.json`), but the experiment view
  plots one axis. First item on the scope-cut list.
- **Graphical profile editing.** Numeric point editing works; dragging a curve
  does not. Third on the scope-cut list.
- **Background sweep jobs.** Sweeps run synchronously, capped at 400 runs.
- **Demo video and measured resume bullets.** Both need a finished, calibrated
  project to point at.

## Scope-cut order if the schedule slips

1. Two-dimensional sweeps and heat maps.
2. Measured-shot calibration interface (keep the files and CLI support).
3. Editable graphical profile control (keep numeric points).
4. Background sweep jobs (run synchronously with a small limit).
5. Temperature profile editing (keep a constant inlet temperature).

Never cut: deterministic artifacts, tests, warnings, or the uniform-puck
flow/extraction core.

## The extension worth building next

Parallel puck regions with different permeability (fidelity level 2). Channelling
is the largest single source of disagreement between this model and a real shot,
and it is the one the current architecture is already shaped for: the flow
solution is a single function of geometry and permeability, so a second region is
a loop rather than a rewrite. Axial thermal cells (level 3) are more work for less
explanatory return until the flow side is honest.
