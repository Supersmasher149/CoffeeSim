# Calibration

The default coefficient set is **uncalibrated**. It produces espresso-shaped
curves because its two dominant knobs were chosen to put the baseline recipe in a
plausible range, not because any measured shot has been fitted. Until that
changes, treat every output as a model result, not a prediction.

Calibration is deliberately a separate, explicit workflow (section 11.3). It is
not something the solver does on its own, and it is never driven by taste notes.

## Collecting a shot

1. Record the baseline recipe and machine setup as precisely as you can:
   basket, dose, grinder, distribution technique, water, roast date.
2. Collect time and beverage mass. Add pressure and refractometer TDS only if
   the equipment actually measures them — a guessed number is worse than a
   missing one.
3. Save it in `assets/measured_shots/` using the layout in that directory's
   README.

## Fitting

4. Choose a small set of coefficients with physical interpretations. Start with
   `kozeny_constant` (sets the overall resistance level) and
   `extraction_rate_ref_s` (sets how fast solids come out). Add
   `initial_porosity` and `dry_permeability_multiplier` only if the shape of the
   flow curve is still wrong after the level is right.
5. Minimise the weighted error of section 11.4 across several shots rather than
   matching one exactly:

```
loss = w_mass * RMSE(beverage_mass_curve)
     + w_time * abs(simulated_stop_time - measured_stop_time)
     + w_tds  * abs(simulated_final_tds - measured_final_tds)
     + regularization_for_nonphysical_coefficients
```

6. Hold back at least one measured shot as a validation case that is never used
   for tuning. A fit that only reproduces its own training shots has told you
   nothing.
7. Commit the fitted coefficients as a **new** file (`default-v2.json`, or an
   id naming the machine) with its dataset reference, date and limitations in
   `provenance`. Never edit a coefficient file in place: the result hash of every
   past run depends on it.

## What a good fit does not license

A calibrated model predicts flow and mass better. It still does not predict
flavour, and it still assumes a uniform puck — a shot that channels in reality
will not be explained by fitting coefficients harder. The honest next modelling
step is parallel flow regions (fidelity level 2), not more tuning.
