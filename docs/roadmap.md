# Roadmap and status

## Where this scaffold sits against the four-week plan

| Week | Focus | Status |
| --- | --- | --- |
| 1 | Skeleton, domain types, profiles, units, water properties, flow-only CLI | Complete |
| 2 | Thermal state, wetting, extraction, mass balances, artifacts, determinism | Complete |
| 3 | Sweep runner, REST server, React controls, synchronized charts, exports | Complete |
| 4 | Calibration, edge cases, CI, README, profiling, demo and portfolio packaging | Partial — see below |

Week 4 is where the remaining work is, and what is left needs something this
project cannot manufacture: real measured shots.

## Done in the calibration milestone

- **Calibration engine.** `espressolab_calibration`: measured-shot loader, the
  weighted loss of section 11.4, and a deterministic Nelder-Mead fit over a
  bounded, named set of physically interpretable coefficients. Wide-ranging
  parameters are searched in log space.
- **`espressolab_cli calibrate`**, with held-out validation shots, a fitted
  coefficient file carrying full provenance, and a JSON report.
- **`espressolab_cli synthesize`**, which writes a measured-shot file from the
  model's own output so the workflow can be exercised without real data. Every
  synthetic file, report and fitted coefficient set is flagged as such.
- **Two-dimensional heat maps** in the experiment view, with a validated
  sequential ramp, per-cell hover, and out-of-range corners rendered as a state
  rather than a magnitude.
- **`espressolab_cli bench`.** A 60-second shot at 100 Hz runs in 0.49 ms
  median — about 2000 simulations per second, 40x inside the section 2.1 budget.

## Done in the background-jobs milestone

- **Background sweep jobs.** `POST /api/v1/sweeps` returns 202 and the sweep runs
  on a worker thread with progress polling and cancellation. The old 400-run
  synchronous cap is gone (the limit is now 20000). Cancelling keeps every run
  that finished, and the partial result exports as CSV like any other.
  The engine gained a progress callback and nothing else: threads stay in the
  server.
- **Graphical profile editing.** Pressure and inlet-temperature profiles are
  draggable curves, with add and remove, snapping, and neighbour fencing that
  preserves the strictly-increasing time rule. The numeric point list is still
  there underneath, because that is the half that survives a scope cut.

## Not done

- **A real calibration.** This is the honest headline: the machinery is built and
  tested, but no measured shot has been fitted, so the default coefficients are
  still uncalibrated placeholders. The recovery test proves the fitter can
  recover known coefficients from synthetic data; it proves nothing about
  espresso. Collecting even three real shots would change what this project can
  claim more than any further code.
- **A calibration view in the dashboard.** The CLI path is complete and the cut
  list says to keep exactly that when the interface goes. With no measured shots
  to drive it, a calibration UI would be a form with nothing to submit; it is
  also the lowest-value remaining item by the guide's own cut order.
- **Demo video and measured resume bullets.** The throughput number now exists;
  the rest waits on a calibrated model.

## Scope-cut order if the schedule slips

1. Two-dimensional sweeps and heat maps. — restored
2. Measured-shot calibration interface (keep the files and CLI support). — CLI
   support built; the dashboard view is still deferred
3. Editable graphical profile control (keep numeric points). — restored
4. Background sweep jobs (run synchronously with a small limit). — restored
5. Temperature profile editing (keep a constant inlet temperature). — never cut

Read as a value ranking (cut the least valuable first), restoring runs in
reverse: 5, 4, 3, 2, 1. Only the calibration interface is outstanding, and it is
blocked on data rather than on effort.

Never cut: deterministic artifacts, tests, warnings, or the uniform-puck
flow/extraction core.

## The extension worth building next

Parallel puck regions with different permeability (fidelity level 2). Channelling
is the largest single source of disagreement between this model and a real shot,
and it is the one the current architecture is already shaped for: the flow
solution is a single function of geometry and permeability, so a second region is
a loop rather than a rewrite. Axial thermal cells (level 3) are more work for less
explanatory return until the flow side is honest.
