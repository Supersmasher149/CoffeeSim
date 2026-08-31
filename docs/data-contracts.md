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
- Grind is spelled **either** as the scalar pair `particle_diameter_um` +
  `particle_spread_factor` **or** as a distribution `puck.grind.bins`, never
  both. A document carrying both is rejected with `CONFLICTING_FIELD`.

### Grind: the two spellings

When `puck.grind` is present the loader derives `particle_diameter_m` (the bins'
Sauter mean, d32) and `particle_spread_factor` from it. Those two fields become
*derived cache values*, not authored input, which has three consequences worth
stating explicitly:

1. **Every existing consumer is unaffected.** The solver, both CFD solvers and
   calibration read the two scalars and need no knowledge of the distribution.
   Only size-resolved extraction walks the bins.
2. **The distribution is what is serialized and hashed.** `dump_recipe_json()`
   emits `grind` and omits the derived scalars, so `recipe_hash()` is taken over
   the authored input rather than over a derivation whose rounding could shift.
   Conversely, when `grind` is absent the key is omitted from the document
   entirely — an unconditional key would have changed the hash of every recipe
   predating the distribution path.
3. **Sweeps retarget the distribution, not the scalar.** Sweeping
   `puck.particle_diameter_um` on a distribution-bearing recipe scales every bin
   by one factor, which leaves the shape (and so the spread) untouched and lands
   d32 exactly on the requested value. Sweeping `puck.particle_spread_factor`
   on such a recipe is refused rather than guessed: there is no shape-preserving
   way to retarget the spread of a fixed distribution.

Bin diameters span 10–2000 µm — wider than the scalar envelope, because real
coffee fines sit at 10–100 µm. The *derived* d32 must still land in 150–800 µm.
Bin diameters are emitted rounded to nanometre resolution so that
load → dump → load → dump is an exact fixed point; without it the µm↔m
round trip drifts by an ulp per save and a re-saved recipe would change hash.

Recipe loading first checks JSON shape and required field types, then the
simulation path calls `Recipe::validate()` for physical ranges and profile
ordering. The loader accepts an omitted `schema_version` as the current recipe
version; `schemas/recipe.schema.json` matches this and does not require the
field either (Audit P6, issue #19). New integrations may still send an
explicit version, but are not required to.

`tests/schemas/schema_contract_check.py` checks representative documents
against both `schemas/*.json` and the built CLI's loader/`validate()` path,
and separately confirms the two constraints the schema cannot express
structurally -- strictly increasing profile times, and `parallel_regions`
area fractions summing to one -- are documented as schema/loader divergences
rather than silent gaps. It needs the third-party `jsonschema` package (not
vendored for the offline build) and is not part of `./scripts/test.sh`; run
it by hand after `pip install jsonschema` in a venv.

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
tested contract change. Identical inputs reproduce the same hash on the same
build. The sample series is formatted at 17 significant digits, so the final
ulp can differ between platform math libraries; the hash is a reproducibility
identity within one toolchain, not a universal cross-platform checksum.

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

## Grinder Specs and Results

`espressolab_cli grind` has its own documents, its own schemas
(`schemas/grinder-spec.schema.json`, `schemas/grinder-result.schema.json`) and
its own loader/serializer in `engine/grind/grinder_io.cpp`. They are deliberately
**not** shot artifacts: they carry no recipe hash, no coefficient hash and no
result hash, because the grinder is outside the shot pipeline and must never
become a hidden dependency of it.

- Every spec field is optional; the loader defaults each to the compiled-in
  `GrinderSpec` value, so `{}` is a valid spec.
- The result's `distribution` object is exactly the shape `recipe.puck.grind`
  takes, so it pastes across unchanged. `grind --out` writes it separately as
  `recipe-grind.json` for that purpose.
- Bin diameters are emitted at nanometre resolution, the same fixed-point rule
  recipe grind bins follow.
- `provenance` travels in the file, recording that the model is unvalidated, so
  a document that outlives this repository still carries its own caveat.

A spec that validates may still produce a distribution a recipe rejects: the
recipe requires a derived d32 in 150–800 µm, and a fine enough burr gap falls
below it. That is intended — the shot correlations do not cover that bed — and
the CLI reports it rather than deferring the failure.

## Beans and the sensory overlay

A recipe may carry an optional `bean` object (`schemas/bean.schema.json`), the
same shape as the standalone documents in `assets/beans/`. `espressolab_core_types`
owns `BeanProfile`; `espressolab_models` owns the correlations.

Three rules govern it:

1. **It owns no physical quantity.** The overlay divides the solids the solver
   already extracted. Mass, flow, TDS, extraction yield, every sample and every
   region summary are bit-identical with and without a bean. A field that would
   change a physical number does not belong in a bean document.
2. **Absent means absent.** `dump_recipe_json()` omits `bean` entirely when there
   is none, and `dump_result_json()`/`dump_summary_json()` omit `flavor`, so every
   recipe and result written before the overlay keeps its bytes and its hash. The
   flavour time series is its own `flavor.csv`; `samples.csv` has a fixed header
   and is never extended.
3. **`bean.description` is metadata, not identity.** `recipe_hash()` erases it
   before hashing, exactly as `coefficient_hash()` erases `provenance`. Editing a
   roaster's cupping note must not change a run's identity or its artifact
   directory. The rest of the bean *is* hashed, because it changes the reported
   flavour.

Attaching a bean therefore changes `recipe_hash` and `result_hash` — the document
differs and the run reports something new — while leaving every beanless run
untouched. Bean values are never calibration targets.

Note that `schemas/shot-result.schema.json` has no `additionalProperties: false`,
which is what makes the additive `flavor` key backward-compatible for existing
consumers. Adding that keyword later would break them.

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
