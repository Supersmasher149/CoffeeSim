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

Rules from 11.3, worth repeating because they are what make the numbers usable:

- Record time and beverage mass first; add pressure and TDS only if the
  equipment measures them.
- Do not tune on taste descriptions during the MVP.
- Fit a small set of coefficients with physical interpretations, minimising a
  weighted error across several shots rather than matching one exactly.
- Hold back at least one shot as a validation case that is never used for
  tuning, and name it in the coefficient file's `provenance`.
