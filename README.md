# EspressoLab

A local engineering workbench that simulates an espresso shot from controllable
brew inputs, plots pressure, temperature, flow, beverage mass, strength and
extraction over time, and lets you compare recipes through reproducible
parameter sweeps.

Deterministic C++20 simulation core, React/TypeScript dashboard, no AI and no
CFD.

## Quick start

```bash
./scripts/build.sh          # cmake + build (dependencies are vendored)
./scripts/demo.sh           # baseline shot, grind sweep, determinism check
./scripts/dev.sh            # tool server + dashboard on http://localhost:5173
```

A baseline shot on an M-series laptop:

```
  termination     target_mass_reached
  shot time       29.03 s
  beverage mass   36.01 g
  brew ratio      1:2.00
  average flow    1.42 ml/s
  peak flow       2.72 ml/s
  TDS             9.09 %
  extraction      18.18 %
  mass residuals  water -4.86e-17 kg, solids 2.17e-18 kg
  clamps          0
  result hash     ba04fea5c54433e0950fc58b8a627ad1a176f177e2ffc15f714e42824b84dcc6
```

`espressolab_cli bench` runs a 60-second shot at 100 Hz in **0.49 ms** median —
about 2000 simulations per second, and 40x inside the 20 ms budget.

## What a grind sweep looks like

`./build/apps/espressolab_cli/espressolab_cli sweep --spec assets/sweeps/grind-size.json`

| particle ⌀ (µm) | shot time (s) | TDS (%) | yield (%) | stop |
| --- | --- | --- | --- | --- |
| 250 | 45.00 | 12.92 | 18.58 | time limit reached |
| 300 | 37.66 | 10.51 | 21.02 | target mass reached |
| 350 | 29.03 | 9.09 | 18.18 | target mass reached |
| 400 | 23.57 | 7.72 | 15.45 | target mass reached |
| 450 | 19.88 | 6.49 | 12.99 | target mass reached |

Finer grind, more resistance, longer shot, stronger and more extracted — until
250 µm chokes badly enough to hit the time limit before reaching 36 g.

## The dashboard

Recipes are edited through draggable pressure and inlet-temperature curves with
the numeric point list underneath, charts share one time cursor, and sweeps run
in the background with live progress and a cancel button — a 1600-run
grind x temperature grid finishes in about four seconds and renders as a heat
map. Every number on screen comes from the native solver; the browser computes
nothing.

## Architecture

```
web (React/TS)  ->  tool_server (REST)  ->  experiment_runner
                                        ->  artifact_io (JSON/CSV/hashes)
                                                    |
                                        espresso_core (state, stepping, termination)
                                                    |
                                        model_library (water, permeability, heat, extraction)
```

Dependencies point inward. The simulation library knows nothing about HTTP,
React, plotting or filesystem locations, so the same model runs unchanged in
unit tests, the CLI, headless sweeps and the browser. The dashboard performs no
authoritative calculations — it posts a recipe and renders what comes back.

See [docs/architecture.md](docs/architecture.md).

## Model assumptions and limitations

A one-dimensional lumped puck: Darcy flow through a Kozeny-Carman-shaped
permeability, a bounded empirical compression curve, one thermal mass, and
bounded first-order extraction into a well-mixed pore liquid. Every equation is
in [docs/model.md](docs/model.md).

What it does **not** do:

- No channelling, no spatial structure, no CFD or particle-resolved model.
- No mapping from a grinder dial number to particle size. Grind is a physical
  input in micrometres.
- **No flavour prediction.** Estimated TDS and extraction yield are engineering
  outputs. Taste depends on compound composition, roast, water chemistry,
  distribution and sensory context this model does not resolve.
- **The default coefficients are uncalibrated.** They put the baseline recipe in
  a plausible range; no measured shot has been fitted. The calibration machinery
  is built and tested — `espressolab_cli calibrate` fits a named set of
  physically interpretable coefficients against measured shots, with held-out
  validation — but it has only ever been run against synthetic data. See
  [docs/calibration.md](docs/calibration.md).

## Reproducibility

Every run records its recipe hash, coefficient hash, solver version, step size
and a SHA-256 result hash over the canonicalised inputs and ordered output
samples. The same inputs produce the same hash, and `scripts/demo.sh` checks it.
Coefficient sets are versioned separately from recipes and from solver code, so
a re-fit never silently changes what a past run meant.

## Command line

```bash
espressolab_cli simulate --recipe <file> [--coefficients <file>] [--out <dir>]
                         [--dt <s>] [--sample-interval <s>] [--quiet]
espressolab_cli sweep    --spec <file> [--out <dir>] [--quiet]
espressolab_cli calibrate --shots <dir> --fit <name,...> [--holdout <id,...>]
                          [--coefficients <file>] [--out <file>] [--report <file>]
espressolab_cli synthesize --recipe <file> [--noise <g>] --out <file>
espressolab_cli bench    [--seconds <s>] [--repeats <n>]

espressolab_cli params     # sweepable recipe parameters
espressolab_cli fit-params # fittable coefficients, with bounds
espressolab_cli version
```

Artifacts land in the layout of section 10.4:

```
outputs/shots/<run-id>/{recipe,coefficients,summary,manifest}.json + samples.csv
outputs/sweeps/<sweep-id>/{sweep.json,runs.jsonl,aggregate.csv,manifest.json}
```

## Tests

```bash
./scripts/test.sh
```

78 test cases, ~14.5k assertions: unit tests for every correlation, whole-shot
integration tests, generated-input property tests, mass-balance invariants, a
step-size convergence test, sweep progress and cancellation tests, and
calibration recovery tests that hide known coefficients in synthetic shots and
require the fitter to find them again. See
[docs/testing.md](docs/testing.md).

## Documentation

| Document | Contents |
| --- | --- |
| [docs/model.md](docs/model.md) | Every equation, coefficient and guardrail |
| [docs/architecture.md](docs/architecture.md) | Component boundaries and the dependency rule |
| [docs/api.md](docs/api.md) | REST endpoints and the error contract |
| [docs/calibration.md](docs/calibration.md) | Fitting coefficients to measured shots |
| [docs/testing.md](docs/testing.md) | What each test layer is for |
| [docs/roadmap.md](docs/roadmap.md) | Status against the four-week plan, and what is not done |
| [schemas/](schemas/) | JSON Schema for recipes, coefficients and results |

## Requirements

CMake 3.20+, a C++20 compiler, and Node 20+ for the dashboard. nlohmann/json,
Catch2 and cpp-httplib are vendored in `third_party/`, so a clean clone builds
offline.
