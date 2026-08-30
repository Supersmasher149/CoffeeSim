# Getting Started

EspressoLab is a local engineering workbench. The native solver, REST server,
and browser dashboard run on your machine; there is no hosted service, account,
or database to configure.

## Requirements

- CMake 3.20 or newer
- A C++20 compiler
- Node.js 20 or newer for the dashboard

The native dependencies are vendored under `third_party/`, so the native build
does not download packages. `scripts/dev.sh` installs the web dependencies into
`web/node_modules` when they are absent.

## Build and Test

From the repository root:

```bash
./scripts/build.sh
./scripts/test.sh
```

Both commands configure the Release build in `build/`. `test.sh` builds first,
then executes the Catch2 test binary. To build a Debug configuration, pass the
build type to either script:

```bash
./scripts/build.sh Debug
./scripts/test.sh Debug
```

## Run a First Simulation

The baseline recipe and coefficient set are versioned assets. Run them through
the CLI and write reproducible artifacts to a fresh directory:

```bash
./build/apps/espressolab_cli/espressolab_cli simulate \
  --recipe assets/recipes/baseline.json \
  --coefficients assets/coefficients/default-v1.json \
  --out outputs/shots/first-run
```

The command prints the terminal metrics and writes these files:

```text
outputs/shots/first-run/
  recipe.json
  coefficients.json
  summary.json
  manifest.json
  samples.csv
```

`manifest.json` records the input hashes, solver version, step settings, and
result hash. The hash is a reproducibility signal: matching inputs and solver
version should produce the same output hash. It is not a security feature.

Run the acceptance demonstration when you want to exercise a baseline shot, a
grind sweep, artifact writing, and the deterministic hash check together:

```bash
./scripts/demo.sh
```

The demo writes to `outputs/shots/baseline`, `outputs/shots/baseline-rerun`, and
`outputs/sweeps/grind-size`. Those locations are intended as generated output;
use another `--out` directory when you need to preserve an earlier run.

## Start the Dashboard

```bash
./scripts/dev.sh
```

The script starts the local REST server on `127.0.0.1:8734` and the Vite
development server on `http://localhost:5173`. Open the Vite URL in a browser.
Stopping the script with `Ctrl-C` also stops the server process it launched.

The dashboard posts recipes to the native solver and renders its response. It
does not reimplement the espresso model in TypeScript. Use the Shot, Measured
Data, References, and Sweeps tabs to move between workflows; tab state remains
available when you switch away and back. On mobile, open the recipe editor from
the Shot tab before changing inputs. The reference-shot panel is contextual
metadata only: its records are not calibration data and do not add telemetry to
a simulated result.

If the dashboard reports that the tool server is unreachable, confirm that the
native build succeeded and run `./scripts/dev.sh` from the repository root. For
direct API use, see [api.md](api.md).

## Explore the CLI

```bash
./build/apps/espressolab_cli/espressolab_cli version
./build/apps/espressolab_cli/espressolab_cli params
./build/apps/espressolab_cli/espressolab_cli sweep \
  --spec assets/sweeps/grind-size.json
```

The CLI also exposes `calibrate`, `synthesize`, `bench`, separate `cfd` and
`cfd3d` commands, and `grind`. The default `simulate` pipeline is the Level 1-3
model used by the REST server and dashboard. `cfd`/`cfd3d` are experimental,
separate solvers; neither alters dashboard results, standard artifacts, or
their hashes. See [model.md](model.md) before interpreting either solver as a
real-world prediction.

A large sweep can use more than one core:

```bash
./build/apps/espressolab_cli/espressolab_cli sweep \
  --spec assets/sweeps/grind-size.json --workers 8
```

`--workers` opts into the parallel batch runner; without it the sweep runs
sequentially exactly as before. `--ring-capacity <n>` overrides the default
`workers * 4` bound on how many finished runs may queue up ahead of the
aggregating consumer, and requires `--workers` to be set too.
`ESPRESSOLAB_SWEEP_WORKERS` and `ESPRESSOLAB_SWEEP_RING_CAPACITY` are the same
two overrides as environment variables, for benchmarking without editing a
command line; a flag wins over its variable. Neither setting changes results:
the parallel path produces the same runs, in the same order, with the same
per-run result hashes as the sequential one.

### Generate a grind distribution

`espressolab_cli grind` turns burr geometry into a particle size distribution.
It sits outside the shot pipeline, like the CFD solvers, and writes its own
files rather than shot artifacts:

```bash
./build/apps/espressolab_cli/espressolab_cli grind \
  --spec assets/grinders/burr-baseline.json --out outputs/grinds/baseline
```

The `recipe-grind.json` it writes is exactly the shape a recipe's `puck.grind`
takes, so it pastes across unchanged. A recipe spells grind *either* as the
scalar `particle_diameter_um` + `particle_spread_factor` pair *or* as a
distribution, never both. Note that a spec which validates can still produce a
distribution a recipe rejects: the recipe requires a derived d32 between 150
and 800 µm, and a fine enough burr gap falls below that. Nothing in the grind
model has been checked against a measured grind.

## Use the Interactive Terminal UI

```bash
./build/apps/espressolab_cli/espressolab_cli tui
```

This launches a guided, form-based frontend to the command set above, on an
interactive macOS or Linux terminal. It covers ten commands; `grind` is
currently file-oriented only and has no TUI form. It calls the same native loaders,
solvers, calibration APIs, and artifact writers as the file-oriented commands
-- not the REST server, and not the CLI itself -- so it produces the same
units, artifacts, and result hashes for the same inputs.

A short walkthrough:

1. Launch the TUI. The home screen groups every workflow under Run
   (`simulate`, `sweep`, `cfd`, `cfd3d`, `bench`), Calibration/Data
   (`calibrate`, `synthesize`), and Info (`params`, `fit-params`, `version`).
2. Use Up/Down to select `simulate` and press Enter. Its form opens with the
   same defaults as the file-oriented command (`assets/recipes/baseline.json`,
   `assets/coefficients/default-v1.json`, `dt=0.01`, `sample-interval=0.05`).
   Press Enter on a field to edit it, Enter again to stop editing, Tab/Down to
   move to the next field, including past the last one to the trailing
   `[ Run ]` action.
3. Press Enter on `[ Run ]` to start the run. Long-running work runs off the
   render loop, so the screen keeps responding; press `c` or Ctrl-C to
   request cancellation. A cancelled single run never writes a partial
   artifact; a cancelled sweep keeps and exports the runs it already
   completed.
4. The result screen shows the same termination state, metrics, warnings, and
   result hash the file-oriented command would print, plus the artifact path
   if you set one. Press Enter or Esc to return to the command list, `q` to
   quit.

Resize and Ctrl-C are handled at every screen; terminal state (raw mode, the
alternate screen buffer, cursor visibility) is always restored on exit,
whether that exit is normal, an error, a cancellation, or Ctrl-C. Running the
TUI with stdin or stdout redirected away from a terminal (for example under
`< /dev/null` or in a CI job) fails immediately with a
`NONINTERACTIVE_TERMINAL` error and a nonzero exit code, rather than entering
the render loop.

## Next Reading

| Need | Document |
| --- | --- |
| Equations, solver fidelity, and limitations | [model.md](model.md) |
| REST endpoints and errors | [api.md](api.md) |
| Recipe, result, and artifact ownership | [data-contracts.md](data-contracts.md) |
| Coefficient fitting and evidence requirements | [calibration.md](calibration.md) |
| Contributing and quality checks | [development.md](development.md) |
