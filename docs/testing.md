# Testing strategy

```bash
./scripts/test.sh                  # build Release and run everything
./build/tests/espressolab_tests "[flow]"   # one tag
./build/tests/espressolab_tests --list-tests
```

Tags: `[units]` `[profile]` `[water]` `[permeability]` `[flow]` `[heat]`
`[extraction]` `[artifacts]` `[integration]` `[invariants]` `[convergence]`
`[sweep]` `[calibration]` `[recovery]` `[property]` `[performance]` `[regions]`
`[axial]` `[cfd]` `[verification]` `[cancellation]` `[tui]` `[cli_workflows]`.

```bash
python3 tests/pty/tui_smoke.py    # separate POSIX PTY smoke matrix for the TUI
```

## What each layer is for

**Unit tests** pin the pieces that have a right answer: conversions, profile
interpolation and its boundaries, water-table knots, the monotone response of
permeability to particle size, zero flow at zero pressure, no heat transfer at
zero temperature difference, no extraction from a dry puck, and stable error
codes and paths from the loaders.

**Integration tests** run whole shots and check behaviour rather than numbers:
the baseline stops on its mass target with no hard warnings, a coarse puck runs
fast and weak, a choked puck stalls with a warning instead of a numerical
failure, pre-infusion delays first drops relative to immediate pressure, and the
same inputs reproduce the same result hash.

**Property and invariant tests** generate 200 valid recipes from a fixed seed
and require that none produce NaN or infinity, that masses stay nonnegative,
that beverage mass and extraction yield never go backwards, and that both mass
balances close to within 1e-9 kg. A second sweep generates 60 random one-to-
eight region partitions and holds the same bounds over each one, because a
partition is an input the hand-written fixtures cannot cover exhaustively.

**Parallel-region tests** cover fidelity level 2 specifically. A uniform single
region has to reproduce the Level 1 aggregate; an asymmetric two-region puck has
to give its higher-permeability region the larger share of integrated flow; the
water and solids residuals have to close across an eight-way unequal split,
where a per-region bookkeeping slip cancels in the aggregate only by luck; and
the `regions` array has to survive into the serialized summary and result with
the configured partition echoed back in recipe order. That last one runs a real
shot and parses the JSON, because the hash test beside it builds its region
summaries by hand and would still pass if the serializer stopped emitting
them.

**Axial cell tests** cover fidelity level 3. One cell has to reproduce the
Level 2 shot; doubling the cell count has to move shot time and yield by a
shrinking amount, because a discretization that does not settle is not resolving
anything; a run stopped mid pre-infusion has to show a wetting front, meaning no
cell drier than the one above it and a real edge somewhere in the column; pore
concentration has to rise and local yield has to fall with depth, which is the
mechanism the lumped puck could not express; the balances have to close at 1, 3,
8 and 32 cells; and the per-cell summary has to reach the serialized artifacts
in screen-to-basket order.

**CFD verification tests** are a different kind of test from everything else
here, and the distinction matters. They do not ask whether the solver describes
espresso; they ask whether it solves the equations it claims to. The discrete
divergence of the total velocity field has to be ~1e-8 1/s against Darcy
velocities four orders of magnitude larger, or the elliptic solve is not
converged and nothing downstream means anything. Water and solute balances have
to close at ~1e-17 kg, which the face-based limiter makes structural rather than
approximate. A uniform bed has to produce an axisymmetric solution, since radial
structure there would be an artefact of the mesh metrics. An isothermal column
is a method-of-exact-solutions check: uniform mobility reduces the pressure
equation to `d2p/dz2 = 0`, so the field must be linear in depth, and the residual
5e-4 departure is identified as the saturation profile by the fact that it
shrinks under refinement. The Darcy velocity is compared against the analytic
value from the same coefficients. Refinement has to converge, and repeated runs
have to reproduce identical fields.

**Convergence tests** run the baseline at 0.02, 0.01 and 0.005 s and require the
change to shrink as the step halves. This is the test that would catch a
scientifically wrong integration scheme hiding behind a plausible curve.

**Calibration recovery tests** are how a fitting routine is checked without real
data. Known coefficients are hidden inside synthetic shots, the fit is started
from a deliberately wrong point, and the test requires it to find the truth again
within 2%. Companion tests check that the fit is deterministic, that held-out
shots never steer it, that coefficients outside the parameter list do not move,
and that synthetic provenance survives into the report and the fitted file. This
validates the machinery; it says nothing about whether the model matches real
espresso.

**Leave-one-out tests** verify that each shot is held out exactly once, aggregate
metrics remain deterministic, acceptance thresholds reject poor held-out fits,
and synthetic, mixed-machine, or incomplete datasets are refused before they
can be described as real-world validation. The tests use model-generated inputs
as fixtures and explicitly avoid treating them as real calibration evidence.

**Cancellation tests** call `Simulator::run`, `CfdSolver::run`, `Cfd3dSolver::run`,
`calibration::fit`, and `calibration::evaluate_shot_loss` with a callback that
always reports "cancelled" and require `ExecutionCancelled` rather than a
completed result -- the checkpoints the TUI's cancel button and Ctrl-C
handling rely on. A companion test in `test_cli_workflows.cpp` requires that a
cancelled `run_simulate` never reaches the artifact writer: the output
directory must not exist afterward. Sweep cancellation and partial-run export
are covered separately, in `test_sweeps.cpp`, since `ExperimentRunner` predates
this contract.

**TUI tests** (`[tui]`, `[cli_workflows]`) are pure and terminal-free: they
call `tui_forms.cpp`'s navigation, field defaults, and validation directly
(never through FTXUI), and `workflows.cpp`'s shared services directly (never
through argv parsing). One test checks that `run_simulate`'s result hash
matches calling `Simulator::run` directly with the same recipe and
coefficients -- the equivalence the legacy CLI and the TUI both depend on now
that they call the same function. What these tests cannot cover -- FTXUI
rendering, raw-mode/alternate-screen lifecycle, resize, and real Ctrl-C
delivery through a terminal's line discipline -- is the separate PTY smoke
matrix's job; see `tests/pty/tui_smoke.py`.

## On golden fixtures

There is deliberately no exact-snapshot golden result. Snapshots make every
beneficial model change look like a regression, and they pass just as happily
when the physics is subtly wrong. The invariants, the mass balances and the
convergence test are what actually guard the model; the result **hash** is used
for reproducibility, not for correctness.

## The acceptance test

`./scripts/demo.sh` is the local acceptance workflow: from a clean clone, run
the baseline recipe, complete a grind-size sweep, export JSON and CSV, and rerun
the same inputs to the same result hash. It is the intended native CI gate for
Linux and macOS, but hosted CI evidence should not be claimed until observed
runs are recorded. See [current-state-and-gaps.md](current-state-and-gaps.md)
for the evidence status.
