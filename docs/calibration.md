# Calibration

The default coefficient set is **uncalibrated**. It produces espresso-shaped
curves because its two dominant knobs were chosen to put the baseline recipe in a
plausible range, not because any measured shot has been fitted. Until that
changes, treat every output as a model result, not a prediction.

Calibration is deliberately a separate, explicit workflow (section 11.3). It is
not something the solver does on its own, and it is never driven by taste notes.

## Running a fit

```bash
espressolab_cli fit-params        # the fittable coefficients, with bounds

espressolab_cli calibrate \
  --shots assets/measured_shots \
  --coefficients assets/coefficients/default-v1.json \
  --fit kozeny_constant,extraction_rate_ref_s \
  --holdout 2026-08-25-machine-3 \
  --out assets/coefficients/fitted-v2.json \
  --report outputs/calibration/report.json
```

A shot can be held out by its `id` or by its filename. A `--holdout` name that
matches nothing is an error rather than a silent no-op: a holdout that quietly
fails would report a fit as validated when it never was.

Only coefficients in `fit-params` can be moved, which is the guardrail against
minimising the loss by turning every knob at once. `kozeny_constant`,
`extraction_rate_ref_s` and `flow_half_saturation_m3_s` are searched in log space
because they span orders of magnitude.

The optimiser is a deterministic Nelder-Mead over the normalised parameter box:
the same shots, starting point and parameter list always produce the same fitted
file. Candidates outside the coefficient validation ranges score a large finite
loss instead of throwing, so the simplex walks away from them rather than
crashing the fit.

## Exercising the workflow without real shots

```bash
espressolab_cli synthesize --recipe assets/recipes/baseline.json \
  --coefficients assets/coefficients/default-v1.json \
  --noise 0.05 --out /tmp/shots/baseline.json
```

This runs the model and writes its output in the measured-shot format, with
optional gaussian scale noise. It exists so the calibration path can be tested
end to end, and so the fitter can be checked by hiding known coefficients in
synthetic data and confirming it recovers them.

**Fitting against synthetic data proves the machinery works and nothing else.**
Every synthetic file is flagged `"synthetic": true`, and that flag propagates
into the calibration report and into the `provenance.limitations` of any
coefficient file fitted from it. Do not remove it.

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
