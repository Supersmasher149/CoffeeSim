# Local API

`espressolab_server` binds `127.0.0.1` (default port 8734) and serves the
endpoints below. It is a local tool server: no accounts, no auth, no cloud
deployment, and no persistence across a server restart. For ownership and
versioning of every request and response document, see
[data-contracts.md](data-contracts.md).

```bash
./build/apps/espressolab_server/espressolab_server --assets assets \
  --references espresso_real_world_refs --port 8734
```

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/v1/health` | Build, schema and solver versions, plus the sweepable parameter list |
| GET | `/api/v1/recipes` | The recipes in `assets/recipes/`, sorted by id |
| GET | `/api/v1/reference-shots` | Read-only published shot metadata, separate from calibration |
| GET | `/api/v1/measured-shots` | Model-ready stored measured-shot catalogue |
| GET | `/api/v1/measured-shots/{id}/compare` | Run one simulation and compare it with stored telemetry |
| POST | `/api/v1/shots` | Validate and execute one simulation |
| GET | `/api/v1/shots/{id}` | Read a completed summary and its samples |
| POST | `/api/v1/cfd3d/runs` | Start an explicit Cartesian 3D CFD run (202) |
| GET | `/api/v1/cfd3d/runs/{id}` | Read 3D run status, summary and diagnostics |
| GET | `/api/v1/cfd3d/runs/{id}/snapshots/{index}` | Read one 3D field snapshot; use `?field=` to select the field |
| POST | `/api/v1/sweeps` | Start a parameter sweep in the background (202) |
| GET | `/api/v1/sweeps` | List this session's sweeps and their progress |
| GET | `/api/v1/sweeps/{id}` | Read status, progress and results |
| POST | `/api/v1/sweeps/{id}/cancel` | Stop a running sweep, keeping finished runs |
| GET | `/api/v1/artifacts/{id}.csv` | Stable CSV for a shot or a sweep |

## Running a shot

```bash
curl -s -X POST localhost:8734/api/v1/shots \
  -H 'Content-Type: application/json' \
  -d "{\"recipe\": $(cat assets/recipes/baseline.json)}"
```

The body accepts `recipe` (required), `coefficients` (defaults to
`assets/coefficients/default-v1.json`) and `solver` (`dt_s`,
`sample_interval_s`). The runtime loader and serializer are the executable
contract; `schemas/shot-result.schema.json` documents the intended external
shape and must be kept synchronized with that behavior.
Recipes may include `parallel_regions`: one to eight objects with
`area_fraction` and `permeability_multiplier`. They share the pressure profile
but evolve independently in the Level 2 solver. The response's `regions` array
contains each final region's beverage mass, integrated flow fraction, TDS, and
extraction yield; the existing samples and summary remain aggregate values.

Recipes may also set `axial_cells`, an integer from 1 to 32 (default 1), which
divides every region into stacked finite-volume cells along the flow direction.
Each region in the response then carries a `cells` array ordered from the screen
side of the puck down to the basket, each entry reporting that cell's final
`saturation`, `temperature_c`, `pore_tds_percent` and `extraction_yield_percent`.
Samples and the CSV export stay aggregate at every cell count.

## Running a Cartesian 3D CFD case

The 3D endpoint is an explicit Level 4b path and does not change the standard
shot response. It accepts the 3D case document directly:

```bash
curl -s -X POST localhost:8734/api/v1/cfd3d/runs \
  -H 'Content-Type: application/json' \
  -d '{"recipe": '"$(cat assets/recipes/baseline.json)"',
       "mesh":{"nx":32,"ny":32,"nz":16},
       "solver":{"snapshot_interval_s":1.0}}'
```

The response is **202 Accepted** with a `run_id` and `poll` path. The case uses
the default coefficient file when `coefficients` is omitted. `material` may be
a scalar multiplier or an object containing a dense x-fastest `values` array;
it represents a permeability multiplier and is bounded to the solver's
supported range. Mesh dimensions and solver controls are bounded by
`schemas/cfd3d-case.schema.json` and the executable C++ loader.

Poll `GET /api/v1/cfd3d/runs/{id}` until `status` is `complete` or `failed`.
Completed status includes the terminal summary and `snapshot_count`. Retrieve
a captured field with:

```text
GET /api/v1/cfd3d/runs/{id}/snapshots/{index}?field=saturation
```

Valid fields are `pressure_pa`, `saturation`, `temperature_k`,
`pore_tds_fraction`, `velocity_x_m_s`, `velocity_y_m_s`, and `velocity_z_m_s`.
Snapshot values are float64 JSON numbers in x-fastest, then y, then z order.
The server retains the four most recent 3D jobs in memory and does not persist
them across restart. Snapshot retrieval returns `409 RUN_NOT_FINISHED` until
the run completes. At most two 3D jobs run concurrently; a third start request
returns `429 TOO_MANY_ACTIVE_RUNS` and creates no job.

Validation performed by the worker after a request has received `202` is an
asynchronous failure, not a later HTTP 4xx. Polling then returns `status:
"failed"` with `error.code`, `error.message`, and `error.path` when the native
validation issue has a path. Unexpected worker failures use `CFD3D_FAILED`.

## Listing Recipes

`GET /api/v1/recipes` returns a `recipes` array sorted by id. A valid entry has
`id`, `name`, and a normalized `recipe` object. If one asset cannot be loaded,
the endpoint keeps the rest of the catalogue available and emits an entry with
`id` and an `error` object instead. API clients must treat these as distinct
shapes and must not attempt to simulate an error entry.

## Reading real-world references

`GET /api/v1/reference-shots` reads the manifest in the directory supplied with
`--references` and returns its records in manifest order. The default directory
is `espresso_real_world_refs`; `scripts/dev.sh` passes it explicitly. These
records preserve source links, setup metadata, grinder metadata, and reported
shot-level values from the source documents.

The catalogue is intentionally separate from `assets/measured_shots/` and the
calibration workflow. The current records have no DE1 time series and no final
shot time, so the response keeps those values null/empty and reports
`telemetry_available: false`. The dashboard presents them as contextual
comparison material, not as validation data or a recipe input.

If the catalogue directory or manifest is unavailable, the endpoint returns a
structured `REFERENCE_CATALOG_NOT_FOUND`, `REFERENCE_MANIFEST_NOT_FOUND`, or
`REFERENCE_MANIFEST_INVALID` error. An individual malformed record is reported
in `load_errors` while valid records remain available.

## Comparing a measured shot

`GET /api/v1/measured-shots` reads model-ready JSON files from
`assets/measured_shots/`. Its top-level fields are `schema_version`,
`measured_shots`, and `count`. Each sorted summary reports `id`, `source_stem`,
`machine`, `date`, `notes`, `synthetic`, and a `final` object whose optional
mass/time/TDS values remain null when unavailable. A malformed file fails the
whole model-ready catalogue with `500 MEASURED_SHOT_LOAD_FAILED`; it is not
silently omitted. This catalogue is distinct from `/reference-shots`, whose
records are contextual published metadata.

Start a comparison with:

```bash
curl -s 'localhost:8734/api/v1/measured-shots/synthetic-shot-974f007b8430/compare?coefficients=default-v1'
```

`coefficients` is an approved asset selector and currently defaults to `default-v1`.
The selector resolves `assets/coefficients/default-v1.json`; that document's
actual coefficient identity is `id: "default"`, `version: "1.0.0"`. They are
not interchangeable identifiers.

The response contains:

| Field | Meaning |
| --- | --- |
| Shot metadata | `id`, `source_stem`, machine/date/notes, and the stored `synthetic` flag |
| `coefficients` | Requested selector plus loaded coefficient `id`, `version`, and hash |
| `simulation` | Termination, solver version, and reproducible result hash |
| `final` | Nullable measured and numeric simulated terminal mass/time/TDS values |
| `loss` | Complete native `LossBreakdown`, including `regularization` and `has_*_measurement` flags |
| `loss_weights` | The fixed weights used for this direct comparison |
| `paired_series` | Measured sample times with measured/simulated mass and measured-minus-simulated residuals |

The endpoint executes the stored recipe once and computes residuals from that
result. It does not call the fitter, mutate coefficients, or write calibration
artifacts. Missing optional measurements are marked by the corresponding
`has_*_measurement` field and are not interpreted as zero. The checked-in
measured-shot assets are synthetic, so their comparisons exercise the workflow
without validating the espresso model.

An unsupported selector or unknown measured-shot id uses the normal 404
contract. A missing or malformed measured-shot,
referenced recipe, or coefficient file under the server's configured asset root
is a server-owned stored-asset failure and returns 500 rather than blaming the
request that selected it.

## Running a sweep

```bash
curl -s -X POST localhost:8734/api/v1/sweeps \
  -H 'Content-Type: application/json' \
  -d "{\"name\":\"grind\",\"baseline\":$(cat assets/recipes/baseline.json),
       \"axes\":[{\"parameter_path\":\"puck.particle_diameter_um\",
                  \"values\":[300,350,400]}]}"
```

`POST` answers **202 Accepted** immediately with a `sweep_id` and a `poll` path;
the sweep runs on a worker thread. Poll `GET /api/v1/sweeps/{id}` for progress:

```json
{ "sweep_id": "sweep-grind-1", "status": "running", "completed": 412,
  "total": 1600, "elapsed_s": 1.06 }
```

`status` is `queued`, `running`, `complete`, `cancelled` or `failed`. The `axes`
and `runs` arrays appear once the sweep reaches `complete` or `cancelled`; a
`failed` sweep carries an asynchronous `error` object with `code` and `message`
instead. As with CFD3D, the original `202` only means that the job was accepted.

`POST /api/v1/sweeps/{id}/cancel` stops a running sweep and **keeps every run it
already finished** — the partial result is a normal sweep result, exportable as
CSV. A single sweep is limited to 20000 runs. The server retains the 32 most
recent sweeps and 128 most recent shots in memory; older IDs return 404.

The server does not enable CORS. The development dashboard uses Vite's local
same-origin proxy, and direct API clients are expected to run locally.

`GET /api/v1/health` returns the sweepable parameter paths, and
`espressolab_cli params` prints the same list. The CLI still runs sweeps
synchronously: it has nothing to poll from.

## Error contract

Every failure answers with the shape from section 12.2:

```json
{
  "error": {
    "code": "OUT_OF_RANGE",
    "message": "recipe.puck.particle_diameter_um must be between 150 and 800 um (received 0)",
    "path": "recipe.puck.particle_diameter_um",
    "details": { "issues": [ ... ] }
  }
}
```

| Status | When |
| --- | --- |
| 400 | Malformed JSON, missing field, unsupported schema version |
| 404 | Unknown run or sweep id |
| 409 | Artifact requested for a sweep that has not finished |
| 413 | Sweep larger than the 20000-run limit |
| 422 | Well-formed but nonphysical or out-of-range input |
| 429 | Two CFD3D jobs are already active; retry after one finishes |
| 500 | Configured stored asset is missing/malformed, or an unhandled server error occurs |

Codes are stable and safe to switch on: `MALFORMED_JSON`, `MISSING_FIELD`,
`UNSUPPORTED_SCHEMA_VERSION`, `MALFORMED_PROFILE_POINT`, `EMPTY_PROFILE`,
`UNORDERED_PROFILE`, `OUT_OF_RANGE`, `NONPHYSICAL_INPUT`, `NONFINITE_INPUT`,
`UNKNOWN_PARAMETER_PATH`, `SWEEP_TOO_LARGE`, `RUN_NOT_FOUND`,
`SWEEP_NOT_FOUND`, `ARTIFACT_NOT_FOUND`, `SWEEP_NOT_FINISHED`, `RUN_NOT_FINISHED`,
`SNAPSHOT_NOT_FOUND`, `UNKNOWN_FIELD`, `EMPTY_SWEEP`,
`EMPTY_SWEEP_AXIS`, `DUPLICATE_SWEEP_AXIS`, `REFERENCE_CATALOG_NOT_FOUND`,
`REFERENCE_MANIFEST_NOT_FOUND`, `REFERENCE_MANIFEST_INVALID`,
`TOO_MANY_ACTIVE_RUNS`, and the measured-shot catalogue/comparison codes.
Stored-asset 500 responses and asynchronous job `error` objects use the same
error vocabulary but do not imply malformed client input.

## Warnings are not errors

A run can succeed and still tell you not to trust it. Warnings arrive in the
result body with a `severity` of `info`, `soft` or `hard`; `hard` means a
numerical guard fired or the model left its supported range. The dashboard marks
each one on the shot timeline. Current codes: `FLOW_CLAMPED`,
`TEMPERATURE_OUT_OF_TABLE`, `TEMPERATURE_STEP_LARGE`, `SATURATION_INVARIANT`,
`NUMERICAL_FAILURE`, `NO_BEVERAGE_PRODUCED`.
