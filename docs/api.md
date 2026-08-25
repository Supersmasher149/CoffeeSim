# Local API

`espressolab_server` binds `127.0.0.1` (default port 8734) and serves the
endpoints of section 12.1. It is a local tool server: no accounts, no auth, no
cloud deployment.

```bash
./build/apps/espressolab_server/espressolab_server --assets assets --port 8734
```

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/v1/health` | Build, schema and solver versions, plus the sweepable parameter list |
| GET | `/api/v1/recipes` | The recipes in `assets/recipes/`, sorted by id |
| POST | `/api/v1/shots` | Validate and execute one simulation |
| GET | `/api/v1/shots/{id}` | Read a completed summary and its samples |
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
`sample_interval_s`). The response follows `schemas/shot-result.schema.json`.

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
`failed` sweep carries an `error` object instead.

`POST /api/v1/sweeps/{id}/cancel` stops a running sweep and **keeps every run it
already finished** — the partial result is a normal sweep result, exportable as
CSV. A single sweep is limited to 20000 runs, and the server retains the 32 most
recent sweeps in memory.

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
| 500 | Unhandled server error (should not happen; report it) |

Codes are stable and safe to switch on: `MALFORMED_JSON`, `MISSING_FIELD`,
`UNSUPPORTED_SCHEMA_VERSION`, `MALFORMED_PROFILE_POINT`, `EMPTY_PROFILE`,
`UNORDERED_PROFILE`, `OUT_OF_RANGE`, `NONPHYSICAL_INPUT`, `NONFINITE_INPUT`,
`UNKNOWN_PARAMETER_PATH`, `SWEEP_TOO_LARGE`, `RUN_NOT_FOUND`,
`SWEEP_NOT_FOUND`, `ARTIFACT_NOT_FOUND`, `SWEEP_NOT_FINISHED`, `EMPTY_SWEEP`,
`EMPTY_SWEEP_AXIS`.

## Warnings are not errors

A run can succeed and still tell you not to trust it. Warnings arrive in the
result body with a `severity` of `info`, `soft` or `hard`; `hard` means a
numerical guard fired or the model left its supported range. The dashboard marks
each one on the shot timeline. Current codes: `FLOW_CLAMPED`,
`TEMPERATURE_OUT_OF_TABLE`, `TEMPERATURE_STEP_LARGE`, `SATURATION_INVARIANT`,
`NUMERICAL_FAILURE`, `NO_BEVERAGE_PRODUCED`.
