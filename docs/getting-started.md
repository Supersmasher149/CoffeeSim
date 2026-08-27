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
does not reimplement the espresso model in TypeScript. The reference-shot panel
is contextual metadata only: its records are not calibration data and do not
add telemetry to a simulated result.

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

The CLI also exposes `calibrate`, `synthesize`, `bench`, and a separate `cfd`
command. The default `simulate` pipeline is the Level 1-3 model used by the
REST server and dashboard. `cfd` is an experimental, separate two-dimensional
solver; it does not alter dashboard results, standard artifacts, or their
hashes. See [model.md](model.md) before interpreting either solver as a
real-world prediction.

## Next Reading

| Need | Document |
| --- | --- |
| Equations, solver fidelity, and limitations | [model.md](model.md) |
| REST endpoints and errors | [api.md](api.md) |
| Recipe, result, and artifact ownership | [data-contracts.md](data-contracts.md) |
| Coefficient fitting and evidence requirements | [calibration.md](calibration.md) |
| Contributing and quality checks | [development.md](development.md) |
