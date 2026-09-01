# Real-shot captures

Records of physically brewed espresso shots, kept for validating the simulator
against reality. **Nothing here is read by the build, the solver, the server or
`espressolab_cli calibrate`.**

The protocol, the researched data sources, and the comparison rules are in
[`docs/real-shot-validation.md`](../../docs/real-shot-validation.md). The
document format is [`schemas/real-shot-capture.schema.json`](../../schemas/real-shot-capture.schema.json),
enforced by `tests/schemas/real_shot_capture_check.py`.

## Contents

| File | What it is |
| --- | --- |
| `TEMPLATE.capture.json` | Copy this per shot. Every field present, every value null |
| `TEMPLATE.telemetry.csv` | Copy this per shot. Header only: `time_s,pressure_bar,flow_ml_s,cumulative_beverage_mass_g,temperature_c` |
| `2026-09-01-pending-capture-01.capture.json` | The first fixture: a reserved slot, `status: pending_capture`. No shot has been brewed. Acquisition instructions are in its `observations.notes` |

## Naming

`<YYYY-MM-DD>-<machine-slug>-<nn>.capture.json`, with the matching
`<same-id>.telemetry.csv`. The `id` field must equal the filename stem.

## Why this is not `assets/measured_shots/`

`espressolab_cli calibrate` reads every `.json` in its shots directory as a
measured shot, and a measured-shot document only carries what the fitter needs.
A capture record carries the opposite: the full setup, the provenance, and an
explicit list of what was **not** measured. Keeping the two apart is what stops
a half-recorded shot from silently entering a fit.

A capture is promoted into `assets/measured_shots/` only when every solver input
is measured and non-null — see "Promoting a capture to a measured shot" in the
validation doc.

## The three statuses

- `measured` — brewed and recorded under the capture protocol here. The only
  status that may ever set `provenance.calibration_eligible`.
- `external_public` — transcribed from a third-party public record. Check
  `provenance.license` before redistributing anything; a null license forbids
  vendoring the raw record.
- `pending_capture` — a slot reserved by the protocol with no shot behind it.
  Every observable is null, and the contract test fails if one is filled in.

## The one rule

Anything not measured stays `null`. Not `""`, not `"TBD"`, not an estimate, and
never a value carried over from somewhere it does not belong — a machine
temperature setpoint is not a measured brew temperature, and a grinder dial
number is not a particle size.
