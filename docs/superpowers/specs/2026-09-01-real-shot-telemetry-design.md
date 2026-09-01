# Next: fill the telemetry gap in the real-world reference shots

## Context

PR #56 landed the real-shot capture format, protocol, and a `pending_capture`
fixture. All four CI jobs pass. The release is blocked on real-shot evidence,
and you've confirmed you have no machine/refractometer — so the only route is
public data.

**During planning I found something that changes the recommendation.** The repo
already contains four curated real shots I had not seen:
`espresso_real_world_refs/` (tracked, committed in `58445b5`, served by the REST
server via `--references` and rendered by `web/src/features/references/ReferenceShotsPanel.tsx`).

They are from Jonathan Gagné's EG-1 vs Niche blooming-espresso experiment — the
same Coffee Ad Astra author from the research brief — and the data is genuinely
good:

| Present | Absent |
| --- | --- |
| Decent DE1+, IMS shower head, Decent 18 g basket | `final_shot_time_s: null` |
| Weber EG-1, SSP Ultra-Low-Fines burrs, setting 4.7 @ 1500 rpm | `timeseries: []` |
| Dose 18.2 g, yield 74.8 g, drip 3.4 g, peak 5.0 bar | Basket diameter, puck depth |
| TDS raw 6.38 % **and filtered 5.69 %**, uncertainty ±0.03 pp | PSD (setting 4.7 is a dial number) |
| EY raw 26.2 % / filtered 23.4 % | Measured brew temperature, water recipe |
| VST refractometer + VST syringe filter, room temperature | Roaster, roast date, storage |
| Full puck prep: deep WDT, Cafelat paper top and bottom, Force Tamper | |

I verified both EY figures reconcile against `tds × yield ÷ dose` (23.38 and
26.22), so the record is internally consistent — a real quality signal.

The manifest states the hole precisely:

> "No telemetry was fabricated. `final_shot_time_s` is null and `timeseries` is
> empty until the original DE1 `.shot` files are parsed."

So the highest-value next step is not importing an arbitrary Visualizer shot —
it is **closing the gap the repo has already scoped for itself**, using the
Visualizer importer as the mechanism. `timeseries_fields` in those records is
already `[time_s, pressure_bar, flow_ml_s, beverage_mass_g, temperature_c]`,
which is the telemetry column set from PR #56 in all but one name.

## The gate this plan turns on

The telemetry for those four specific 2021-03-02 shots may or may not be
publicly retrievable. I probed this and it is genuinely uncertain:

- `GET https://visualizer.coffee/api/shots/<uuid>` works anonymously and returns
  full telemetry (I pulled a 351-sample shot successfully).
- But `?user_id=`, `?q=`, `/shots/search`, and `/users/<id>/shots` are ignored or
  404 anonymously — so **locating Gagné's specific shots by author is not solved.**

**Step 1 is therefore a timeboxed acquisition spike, not construction.**

### Step 1 — Acquisition spike (do this first, ~30 min, read-only)

Try, in order, to obtain telemetry for `20210302T094131.shot` and its three siblings:

1. The `experiment_log_url` Google Sheet already recorded in each file's `source`.
2. The `article_url` on coffeeadastra.com for linked raw shot files.
3. Visualizer, for shots matching the coffee (`Asotbilbao`), grinder (EG-1 SSP
   ULF), and date (2021-03-02).

Then **stop and report** which of the two paths below is live. Do not build
before this resolves.

### Step 2A — If the four shots' telemetry is obtainable (preferred)

- Add a DE1 `.shot` / Visualizer-JSON → telemetry parser as a stdlib-only Python
  tool (`tools/import_shot_telemetry.py`). Python, not C++: the repo must build
  offline from a clean clone, and an HTTPS client would mean a new vendored
  dependency. Precedent is the existing stdlib-only tooling in `tests/schemas/`,
  `tests/pty/`, `tests/cli/`.
- Populate `timeseries` and `final_shot_time_s` in the four
  `espresso_real_world_refs/*.json` records, updating each `data_quality` block
  to say the telemetry is now populated and from where.
- Extend `tests/unit/test_reference_io.cpp` for the now-non-empty series.

### Step 2B — If it is not obtainable (fallback)

- Point the same importer at a fully public Visualizer shot and emit an
  `external_public` capture record + telemetry CSV in the PR #56 format.
- Record in `espresso_real_world_refs/*.json` that the telemetry could not be
  retrieved, and what would be needed — so the gap is documented rather than
  left silently open.

## Schema refinements this real data forces

Applying the PR #56 format to actual records surfaced three gaps. These are
small, and each is a correction the real data proved necessary:

1. **`tds_method.readings_percent` must not be mandatory for `external_public`.**
   The current rule in `tests/schemas/real_shot_capture_check.py` ("tds_percent
   recorded but readings empty") is right for a first-party `measured` shot, but
   a public source publishes an aggregate. Scope the rule to `status == "measured"`.
2. **Raw and filtered TDS need separate fields.** Gagné reports both plus an
   uncertainty; the schema currently has a single `tds_percent` and a boolean
   `filtered`. Add `tds_raw_percent`, `tds_filtered_percent`,
   `tds_uncertainty_percent_points`, and make `tds_percent` name which one is
   authoritative.
3. **`grinder.dial_setting` must be stringified on import.** These files store it
   as the number `4.7`; the capture format requires a string, deliberately, so it
   can never be read as microns. The importer converts verbatim — `"4.7"`, never
   a unit conversion.

## Files

| File | Change |
| --- | --- |
| `tools/import_shot_telemetry.py` | New. Stdlib only. Fetch/parse → telemetry rows |
| `espresso_real_world_refs/*.json` (4) | Populate `timeseries`, `final_shot_time_s`, update `data_quality` |
| `espresso_real_world_refs/manifest.json` | Update the "no telemetry" note once it is no longer true |
| `schemas/real-shot-capture.schema.json` | The three TDS/dial refinements above |
| `tests/schemas/real_shot_capture_check.py` | Scope the readings rule to `measured`; cover raw/filtered TDS |
| `tests/unit/test_reference_io.cpp` | Non-empty `timeseries` cases |
| `docs/real-shot-validation.md` | Record the outcome and correct the source table |

## Reuse — do not rebuild

- `calibration::compare_shot_result()` (`engine/calibration/loss.cpp:111`,
  declared `include/espressolab/calibration.hpp:174`) is already the shared
  authority for scoring a result against a measured shot, used by the server's
  `/api/v1/measured-shots/<id>/compare` route. Any comparison work reuses it.
- `interpolate_beverage_mass_g()` already handles off-grid sample times.
- `engine/reference_io/reference_io.cpp` already loads these records.

## Verification

- `python3 tests/schemas/real_shot_capture_check.py` — passes.
- `./build/tests/espressolab_tests "[references]"` and `"[calibration]"` — pass.
- `./scripts/demo.sh` — determinism/hash unchanged. Nothing here touches the
  solver, but the reference records are server-served, so confirm it.
- `npm --prefix web run test:coverage` — the ReferenceShotsPanel renders the
  now-populated records.
- Re-run the importer on the same input and diff: byte-identical output.

## Explicitly out of scope

Per your standing brief, and unchanged by this plan: no performance work, no
gamification, no new CFD3D features, no dashboard work beyond what a populated
reference record renders. Your ~2000 lines of uncommitted working-tree changes
stay untouched, as you asked.

**No calibration, and no validation claim.** Even with telemetry, these shots
lack basket diameter, puck depth, PSD, and a measured brew temperature, so they
cannot be simulated without inventing inputs. The honest outcome is real curves
for shape comparison and documented provenance — not a calibrated model.
