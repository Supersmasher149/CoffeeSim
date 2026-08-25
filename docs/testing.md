# Testing strategy

```bash
./scripts/test.sh                  # build Release and run everything
./build/tests/espressolab_tests "[flow]"   # one tag
./build/tests/espressolab_tests --list-tests
```

Tags: `[units]` `[profile]` `[water]` `[permeability]` `[flow]` `[heat]`
`[extraction]` `[artifacts]` `[integration]` `[invariants]` `[convergence]`
`[sweep]` `[calibration]` `[recovery]` `[property]` `[performance]`.

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
balances close to within 1e-9 kg.

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

## On golden fixtures

There is deliberately no exact-snapshot golden result. Snapshots make every
beneficial model change look like a regression, and they pass just as happily
when the physics is subtly wrong. The invariants, the mass balances and the
convergence test are what actually guard the model; the result **hash** is used
for reproducibility, not for correctness.

## The acceptance test

`./scripts/demo.sh` is the shipping gate from section 2.2: from a clean clone,
run the baseline recipe, complete a grind-size sweep, export JSON and CSV, and
rerun the same inputs to the same result hash. CI runs it on Linux and macOS.
