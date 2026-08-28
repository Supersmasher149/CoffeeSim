# Data Contracts

EspressoLab keeps its public inputs and outputs intentionally simple: JSON for
structured documents and CSV for sampled or aggregate data. This guide explains
which layer owns each document and how to change a contract safely.

## Contract Authority

The C++ loaders and serializers are the executable contract:

| Data | Domain owner | Runtime boundary | Serialized form |
| --- | --- | --- | --- |
| Recipe | `Recipe` in `types.hpp` | `artifact_io::load_recipe_json` | Recipe JSON and a normalized artifact copy |
| Coefficients | `ModelCoefficients` in `types.hpp` | `artifact_io::load_coefficients_json` | Coefficient JSON and a normalized artifact copy |
| Simulation result | `ShotResult` in `result.hpp` | `artifact_io::dump_result_json` | REST response, summary, manifest, and samples CSV |
| 3D CFD case/result | `Cfd3dCase` and `Cfd3dResult` in `cfd3d_artifact_io.hpp` | `cfd3d_artifact_io` | 3D REST status/snapshots, JSON case, summary, manifest, samples CSV, and `ELF3D-1` fields |
| Sweep | `SweepSpec` and `SweepResult` in `experiment.hpp` | `artifact_io_sweep` and the server | Sweep JSON, JSONL runs, aggregate CSV, and REST status |
| Measured shot/comparison | `calibration::MeasuredShot`, `MeasuredSample`, and `LossBreakdown` in `calibration.hpp` | `calibration::io` plus server comparison translation | Stored measured-shot JSON, catalogue summaries, and one-simulation comparison responses |
| Reference catalogue | `reference_io::Catalogue` | `reference_io::load_directory` | `GET /api/v1/reference-shots` response |

`schemas/` documents the intended JSON exchange formats and is useful to tools
outside the process. The runtime currently validates through C++ loaders and
domain validation rather than automatically applying a JSON Schema validator.
When schema text and runtime behavior differ, the loader and serializer describe
what the application actually accepts or emits; treat the difference as a bug to
resolve, not as an alternative compatibility policy.

## Units and Boundaries

The native model uses SI units internally. Recipe and result documents use the
dashboard-facing units named in their fields:

| Quantity | External unit | Internal unit |
| --- | --- | --- |
| Dose and beverage mass | g | kg |
| Basket diameter and puck depth | mm | m |
| Particle diameter | um | m |
| Pressure | bar | Pa |
| Temperature | C | K |
| Flow | ml/s | m3/s |
| TDS and extraction yield | percent | fraction |

Convert only at an explicit boundary. Do not add browser-side formulae for
authoritative metrics; the dashboard may derive display-only presentation values
from samples but must render model outputs returned by the native solver.

## Recipes

A recipe supplies puck geometry, particle inputs, pressure and temperature
profiles, termination settings, and optional fidelity controls:

- `parallel_regions` partitions the basket laterally. Fractions must sum to one.
- `axial_cells` selects one to 32 stacked cells in every region. One cell is the
  lumped default.
- Profiles are ordered `[time_s, value]` pairs and are linearly interpolated.

Recipe loading first checks JSON shape and required field types, then the
simulation path calls `Recipe::validate()` for physical ranges and profile
ordering. The current loader accepts an omitted `schema_version` as the current
recipe version even though the schema requires the field. New integrations should
always send an explicit version and should not rely on that fallback.

## Coefficients

Coefficient files are separate from recipes because a recipe describes an
experiment while a coefficient set describes the chosen empirical model. A
coefficient document has an `id`, `version`, and a complete `values` object. The
loader requires every model value; partial overrides are not supported.

The default coefficient file is uncalibrated. Its values produce a plausible
engineering baseline but are not evidence that the model predicts real shots.
Calibration output must retain its data provenance and limitations. See
[calibration.md](calibration.md).

Asset selectors are routing names, not coefficient identity. In particular,
`default-v1` selects `assets/coefficients/default-v1.json`, whose serialized
coefficient identity is `id: "default"`, `version: "1.0.0"`. Responses expose
the loaded identity and hash rather than rewriting them to the selector.

## Results and Artifacts

`ShotResult` is emitted as a JSON result for the REST API. File-oriented CLI
runs split the same result into separate artifacts:

| File | Purpose |
| --- | --- |
| `recipe.json` | Normalized input recipe used for the run |
| `coefficients.json` | Normalized coefficient set used for the run |
| `summary.json` | Terminal metrics, diagnostics, warnings, and final region state |
| `manifest.json` | IDs, schema and solver versions, hashes, and solver controls |
| `samples.csv` | Ordered aggregate time series |

For an axially resolved recipe, each final region has a `cells` array ordered
from the screen side down to the basket. Cell summaries include saturation,
temperature, pore TDS, and extraction yield. The sample series remains an
aggregate, not a per-cell time history.

The result hash covers canonicalized inputs, solver controls, ordered samples,
and final region summaries. Changing serialization order or the set of hashed
fields changes reproducibility identity and must be treated as a deliberate,
tested contract change.

## Cartesian 3D CFD

The Level 4b case is a JSON object containing the shared `recipe`, optional
`coefficients`, a Cartesian `mesh` (`nx`, `ny`, `nz`), solver controls, and an
optional cell-centred permeability multiplier `material`. The mesh is
x-fastest in storage, followed by y and z. Dimensions are bounded at runtime
to 128 x 128 x 256 and 262144 cells total.

The solver emits a separate `Cfd3dResult` summary with terminal metrics,
diagnostics, warnings, final fields, and circular cut-cell geometry metadata.
Optional snapshots contain seven float64 fields: pressure, saturation,
temperature, pore TDS, and the three velocity components. Snapshot fields use
the same x-fastest ordering and are served directly by the explicit 3D REST
API.

File-oriented 3D runs additionally write:

| File | Purpose |
| --- | --- |
| `case.json` | Normalized 3D case and material input |
| `summary.json` | Terminal 3D metrics and diagnostics |
| `manifest.json` | 3D schema versions, hashes, solver controls, and field metadata |
| `samples.csv` | Ordered aggregate time series |
| `mesh.json` | Cut-cell geometry and classification metadata |
| `fields.elf3d` | Appendable little-endian float64 snapshot chunks |
| `index.json` | Snapshot offsets, times, fields, and dimensions |

The 3D case/result schemas and field format are versioned independently from
the standard shot result. `ELF3D-1` is little-endian, uncompressed, and has no
implicit browser-side calculations.

## Measured Shots

A measured-shot document owns the recorded recipe, ordered time/mass series,
optional pressure samples, optional final mass/time/TDS, setup metadata, and the
`synthetic` evidence flag. `calibration::io` is the executable loader. The
catalogue is model-ready input and is distinct from the published reference
catalogue, whose incomplete telemetry cannot satisfy this contract.

A comparison response combines measured-shot metadata and final observations,
simulation identity, coefficient selector plus loaded id/version/hash, and a
complete `LossBreakdown`, plus a mass-only `paired_series` evaluated at measured
sample times. It is
an evaluation artifact, not calibration output: no coefficient is optimized and
no fit provenance is created. Missing optional measurements are represented by
availability flags and nullable final values; they do not become zero-valued
observations. Paired residuals use `measured - simulated` sign convention.

## Sweeps and References

A sweep specifies a baseline recipe, coefficients, solver configuration, and
one or more axes. The runner executes the Cartesian product in stable declared
order. The server retains sweep state in memory for the current process only;
restarting it discards running and completed jobs. Aggregate export currently
uses its documented fixed columns, so callers should not assume that arbitrary
metric projection is available from the `output_metrics` field.

Reference records preserve published setup and terminal measurement metadata.
They are intentionally separate from measured-shot calibration files. The
current catalogue does not provide DE1 time series or complete shot timing, so it
must not be used as calibration or validation input.

## Contract Change Procedure

1. Update the C++ owner and define validation, default, and compatibility rules.
2. Update loaders, serializers, manifests, hash behavior, and REST translation.
3. Update schemas, TypeScript types, and user-facing API documentation.
4. Add tests that round-trip the new field and reject malformed or incompatible
   input.
5. Update example assets only when they remain meaningful for the new contract.
6. State the schema or solver-version consequence in the change description.

See [development.md](development.md) for build and test commands that support
this workflow.
