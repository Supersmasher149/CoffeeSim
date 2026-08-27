# Real-world reference panel

## Scope

Expose the four records in `espresso_real_world_refs/` as a read-only dashboard
reference catalogue. These records are published/observed shot metadata, not
model-ready measured-shot inputs. The feature must not use them for calibration,
simulation, telemetry overlays, or recipe generation.

## Goals

- Make the four references visible in the local dashboard.
- Preserve source URLs, setup metadata, grinder metadata, and all reported
  observed values.
- Compare the references side by side.
- Add an optional current-model column when a simulation result is active.
- Make missing telemetry and the non-validation status impossible to miss.
- Keep the existing simulation and calibration workflows unchanged.

## Non-goals

- Parsing original DE1 `.shot` files.
- Inferring final shot time, flow, pressure traces, or beverage-mass series.
- Treating the records as `MeasuredShot` files.
- Fitting coefficients or emitting validation claims.
- Loading a reference into the recipe editor.
- Overlaying references on time-series charts.

## Data Flow

The reference files remain in `espresso_real_world_refs/` unchanged. The server
receives the directory through `--references`, defaulting to that path for the
repository's normal local launch. `scripts/dev.sh` passes the path explicitly.

`GET /api/v1/reference-shots` reads `manifest.json`, follows its ordered file
list, and returns a catalogue response. A dedicated reference I/O boundary owns
the reference document shape; the calibration loader is not reused because these
records intentionally do not satisfy the measured-shot contract.

The dashboard requests the catalogue independently from health, recipes, and
simulation state. A reference fetch failure therefore does not prevent the
simulation dashboard from loading or running.

## API Contract

The successful response is:

```json
{
  "schema_version": "1.0",
  "telemetry_available": false,
  "limitation": "Shot-level metadata is reported; DE1 time series and final shot times are unavailable.",
  "references": [
    {
      "id": "real_gagne_eg1_01",
      "file": "gagne_eg1_01.json",
      "source": {},
      "setup": {},
      "grinder": {},
      "observed": {},
      "telemetry_available": false
    }
  ],
  "load_errors": []
}
```

The `source`, `setup`, `grinder`, and `observed` objects retain the source
fields and values, including null values and the empty `timeseries` array where
present. The server may add display metadata, but it must not replace null with
zero or manufacture measurements.

If the reference directory is unavailable, the endpoint returns the existing
structured API error contract with `REFERENCE_CATALOG_NOT_FOUND`. If the
manifest is missing or malformed, it returns `REFERENCE_MANIFEST_NOT_FOUND` or
`REFERENCE_MANIFEST_INVALID`. If an individual listed file is malformed or
missing, the endpoint still returns valid records and adds
`{ "file", "code", "message" }` to `load_errors`. The dashboard renders that as
a non-blocking notice.

The endpoint does not alter `/api/v1/recipes`, `/api/v1/shots`, calibration CLI
behavior, or the result schema.

## Dashboard Behavior

`ReferenceShotsPanel` appears below the run comparison area and above sweeps.
It renders a horizontally scrollable comparison table. On desktop the first
column is `Current model` when a result is active, followed by the four
references; without an active result, the four references fill the table. On
narrow screens the table remains usable through horizontal scrolling.

Reference columns show:

- Shot identity, grinder model and setting.
- Dose, final beverage mass, target brew ratio, and drip mass.
- Peak pressure, filtered TDS, filtered extraction, and shot time.
- Article and experiment-log links.

The active model column shows solver shot time, beverage mass, TDS, extraction,
and brew ratio. Values not available for a side are rendered as `not reported`,
never as zero. Reference values are labeled as reported measurements; model
values are labeled as current solver output.

Each record also exposes a compact setup detail block containing machine,
coffee/origin/process, profile, and bloom time. The panel displays a persistent
warning that this is contextual comparison only, not validation or calibration,
because the reference recipes and setup are not represented as dashboard
recipes and the telemetry is incomplete.

The panel does not add reference records to the existing run pinning state and
does not pass them to chart components.

## Implementation Units

- `include/espressolab/reference_io.hpp`: reference catalogue types and loader
  interface.
- `engine/reference_io/reference_io.cpp`: manifest/file loading, preservation of
  JSON values, and per-file error collection.
- `engine/reference_io/CMakeLists.txt`: library target linked by the server and
  tests.
- `apps/espressolab_server/main.cpp`: `--references` parsing and catalogue
  endpoint.
- `scripts/dev.sh`: explicit local reference path.
- `web/src/api/types.ts` and `web/src/api/client.ts`: typed response contract.
- `web/src/features/references/ReferenceShotsPanel.tsx`: catalogue rendering and
  current-model comparison.
- `web/src/App.tsx` and `web/src/styles.css`: data loading and responsive visual
  treatment.
- `docs/api.md` and `README.md`: endpoint, launch, and provenance documentation.

## Verification

Native tests will cover:

- Four manifest-ordered records load from the supplied catalogue.
- Source URLs and reported metrics survive loading.
- Null final shot time and empty timeseries remain unavailable.
- Missing or malformed individual records appear in `load_errors`.
- The reference loader does not mark records as calibration-ready.

The local verification sequence is:

```bash
./scripts/test.sh
cd web && npm run typecheck
cd web && npm run build
```

An API smoke test starts the server with `--assets assets --references
espresso_real_world_refs`, requests `/api/v1/reference-shots`, and verifies the
four IDs, the global telemetry limitation, and a successful response with an
active simulation available to the dashboard.

## Error and Provenance Rules

- Reference files are read-only through this feature.
- Missing telemetry remains explicit and is never guessed.
- Reference values must retain their source context and links.
- Reference records are never included in calibration directory workflows.
- A reference endpoint failure is isolated from simulation failures in the UI.
