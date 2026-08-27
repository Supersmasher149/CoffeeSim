# EspressoLab

A local engineering workbench that simulates an espresso shot from controllable
brew inputs, plots pressure, temperature, flow, beverage mass, strength and
extraction over time, and lets you compare recipes through reproducible
parameter sweeps.

Deterministic C++20 simulation core, React/TypeScript dashboard, no AI, and a
separate experimental CFD solver.

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
  result hash     80156b2ee1185e238f73145c5080cc4e89cdec2f26a82b30a2778362c4243c06
```

`espressolab_cli bench` runs a 60-second shot at 100 Hz in **0.49 ms** median —
about 2000 simulations per second, and 40x inside the 20 ms budget.

## What a grind sweep looks like

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/grind-sweep-dark.svg">
  <img src="docs/images/grind-sweep-light.svg" alt="Grind-size sweep showing finer particles produce longer shots, higher TDS, and higher extraction until the finest puck reaches the time limit.">
</picture>

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
map.

A cross-section panel replays the shot beside the charts: water entering the
bed, the saturation level rising while nothing reaches the cup, then the stream
leaving the spout and the cup filling and darkening as TDS climbs. Lateral
regions are drawn to scale by area fraction, so a channelled puck shows its
narrow high-permeability region taking most of the flow. The panel plays on its
own transport or follows the shared chart cursor.

The dashboard also includes a read-only catalogue of real-world reference shots
under `espresso_real_world_refs/`. It preserves the reported setup and terminal
measurements, links back to the source experiment, and clearly marks the records
as metadata only: the supplied files contain no DE1 time series or final shot
times and are not used for calibration or validation.

Every simulation number on screen comes from the native solver. The one piece of browser
arithmetic is the cross-section's spout, which differences beverage mass between
neighbouring samples to know how fast the cup is filling — the sampled flow is
the Darcy flow *into* the bed, which is a different number until the pores are
full.

## Architecture

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/architecture-dark.svg">
  <img src="docs/images/architecture-light.svg" alt="EspressoLab architecture: the web dashboard, CLI, and tests drive one simulation core, which depends on the model library and outputs artifacts.">
</picture>

```
web (React/TS)  ->  tool_server (REST)  ->  experiment_runner
                                         ->  artifact_io (JSON/CSV/hashes)
                                         ->  reference_io (published metadata)
                                                     |
                                         espresso_core (state, stepping, termination)
                                                     |
                                         model_library (water, permeability, heat, extraction)

CLI and CFD tests  ->  cfd (separate Level 4 solver)  ->  espresso_core + model_library
```

Dependencies point inward. The simulation library knows nothing about HTTP,
React, plotting or filesystem locations, so the same model runs unchanged in
unit tests, the CLI, headless sweeps and the browser. The dashboard performs no
authoritative calculations — it posts a recipe and renders what comes back.

See [docs/architecture.md](docs/architecture.md).

## Model assumptions and limitations

A lateral parallel-region puck divided into stacked axial finite-volume cells:
Darcy flow through a Kozeny-Carman-shaped permeability with the cells in
hydraulic series, a bounded empirical compression curve, and cell-local thermal
and extraction states. `axial_cells` defaults to 1, which is the lumped puck.
Every equation is in [docs/model.md](docs/model.md).

What it does **not** do:

- The default pipeline has no radial structure inside a region and no dynamic
  channelling. A separate 2D axisymmetric CFD solver (below) adds the radial
  coordinate; it does not feed the dashboard or the artifacts.
- **No pore-resolved simulation.** The CFD solver's momentum closure is Darcy at
  the representative-elementary-volume scale. Pore geometry is not meshed, so
  nothing here is particle-resolved or a pore-scale DNS.
- **Nothing is validated against a real shot**, CFD included. Verification and
  validation are different claims; see below.
- No mapping from a grinder dial number to particle size. Grind is a physical
  input in micrometres.
- **No flavour prediction.** Estimated TDS and extraction yield are engineering
  outputs. Taste depends on compound composition, roast, water chemistry,
  distribution and sensory context this model does not resolve.
- **The default coefficients are uncalibrated.** They put the baseline recipe in
  a plausible range; no measured shot has been fitted. The calibration machinery
  is built and tested — `espressolab_cli calibrate` fits a named set of
  physically interpretable coefficients against measured shots, with held-out or
  leave-one-shot-out validation — but it has only ever been run against synthetic data. See
  [docs/calibration.md](docs/calibration.md).

## The CFD solver

`espressolab_cli cfd` runs a two-dimensional axisymmetric finite-volume solver
for two-phase flow through the puck: an elliptic pressure solve for
`div(lambda_t grad p) = 0` on an (r, z) mesh, with IMPES saturation transport
and donor-upwinded enthalpy and solute advection. It is a separate entry point.
The Level 1-3 pipeline, its artifacts and its hashes are unaffected by it.

```bash
espressolab_cli cfd --recipe assets/recipes/channelled.json \
  --radial 10 --axial 16 --field saturation
```

It reports its own verification with every run:

```
  max |div u_t|   1.250e-07 1/s
  water residual  -2.776e-17 kg
  solids residual -4.770e-18 kg
  saturation clamps 0
```

Because the mesh carries a radial coordinate, a channelled recipe is no longer
*told* how the flow splits: the solver is given only where the permeability
differs and resolves the radial pressure gradient and cross-flow itself.

The momentum closure is Darcy at the representative-elementary-volume scale,
which is what CFD means for a medium whose pores are represented statistically.
It is not a pore-resolved simulation, and it is verified but not validated. See
[docs/model.md](docs/model.md).

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
                           [--leave-one-out]
espressolab_cli synthesize --recipe <file> [--noise <g>] --out <file>
espressolab_cli cfd      --recipe <file> [--coefficients <file>]
                         [--radial <n>] [--axial <n>] [--dt <s>]
                         [--field pressure|saturation|temperature|tds]
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

115 test cases, 17,385 assertions: unit tests for every correlation, whole-shot
integration tests, generated-input property tests, mass-balance invariants, a
step-size convergence test, sweep progress and cancellation tests, parallel-region
balance and serialization tests, axial grid-refinement and wetting-front tests, CFD verification tests
(divergence, exact solutions, mesh convergence, conservation),
calibration recovery tests, and deterministic leave-one-out validation tests. See
[docs/testing.md](docs/testing.md).

## Documentation

| Document | Contents |
| --- | --- |
| [docs/getting-started.md](docs/getting-started.md) | Build, test, simulate, run the dashboard, and locate artifacts |
| [docs/development.md](docs/development.md) | Contributor workflow, module ownership, build variants, and quality checks |
| [docs/data-contracts.md](docs/data-contracts.md) | Recipe, coefficient, result, sweep, and reference data ownership |
| [docs/model.md](docs/model.md) | Every equation, coefficient and guardrail |
| [docs/architecture.md](docs/architecture.md) | Component boundaries and the dependency rule |
| [docs/api.md](docs/api.md) | REST endpoints and the error contract |
| [docs/calibration.md](docs/calibration.md) | Fitting coefficients to measured shots |
| [docs/testing.md](docs/testing.md) | What each test layer is for |
| [docs/roadmap.md](docs/roadmap.md) | Status against the four-week plan, and what is not done |
| [docs/current-state-and-gaps.md](docs/current-state-and-gaps.md) | Evidence-based implementation status and open gaps |
| [schemas/](schemas/) | JSON Schema for recipes, coefficients and results |

## Requirements

CMake 3.20+, a C++20 compiler, and Node 20+ for the dashboard. nlohmann/json,
Catch2 and cpp-httplib are vendored in `third_party/`, so a clean clone builds
offline.
