# Measured shots

Real reference shots for the calibration workflow in section 11.3. Nothing here
is used by the default build; calibration is deliberately a separate, explicit
step.

One file per shot, `YYYY-MM-DD-<machine>-<n>.json`:

```json
{
  "schema_version": "1.0",
  "recipe": "../recipes/baseline.json",
  "machine": "describe the machine, basket and grinder",
  "date": "2026-08-25",
  "series": { "time_s": [], "beverage_mass_g": [], "pressure_bar": [] },
  "final": { "beverage_mass_g": null, "shot_time_s": null, "tds_percent": null },
  "notes": "distribution technique, water, roast date"
}
```

Fit against them with:

```bash
espressolab_cli calibrate --shots assets/measured_shots \
  --fit kozeny_constant,extraction_rate_ref_s --leave-one-out \
  --report outputs/calibration/leave-one-out.json \
  --out assets/coefficients/fitted-v2.json
```

Write fitted coefficients and reports somewhere else: every `.json` in this
directory is read as a measured shot.

Records of *physically brewed* shots live in `assets/real_shots/` instead, in a
richer format that also states what was not measured. They are deliberately not
readable here; see `docs/real-shot-validation.md` for the capture protocol and
for when a capture may be promoted into this directory.

Rules from 11.3, worth repeating because they are what make the numbers usable:

- Record time and beverage mass first; add pressure and TDS only if the
  equipment measures them.
- Do not tune on taste descriptions during the MVP. This still holds now that
  bean profiles exist: the sensory overlay (`assets/beans/README.md`) is a
  separate, uncalibrated layer and must never become a calibration target.
- Fit a small set of coefficients with physical interpretations, minimising a
  weighted error across several shots rather than matching one exactly.
- Hold back at least one shot as a validation case that is never used for
  tuning, and name it in the coefficient file's `provenance`.

## Leave-one-out validation

For a small real dataset, `--leave-one-out` fits once per shot and holds out a
different shot each time. It requires at least three non-synthetic files with
unique IDs, the same nonempty `machine` description, at least two mass samples,
and a final shot time. TDS and pressure remain optional. The command always
requires `--report`; it writes `--out` only if validation passes.

The pass criteria are median held-out mass RMSE at most 1 g, median time error
at most 2 s, and no fold above twice either limit. When every shot has TDS, the
median TDS error must also be at most 0.25 percentage points with no fold above
0.5. Pressure RMSE is reported as a diagnostic, not fitted: pressure is a recipe
input in this model, not a pump-dynamics prediction.
