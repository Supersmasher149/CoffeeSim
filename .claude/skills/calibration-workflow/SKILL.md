---
name: calibration-workflow
description: Drives espressolab_cli's calibrate/synthesize/fit-params commands and scripts/calibration_demo.sh, the synthetic-recovery sanity check that fits known coefficients back out of generated data (CI's "Calibration recovery" step). Use when fitting coefficients to measured shots, changing the fittable coefficient set or loss function, or verifying the calibration machinery still recovers ground truth.
---

# Calibration Workflow

## Key Commands

- `espressolab_cli fit-params` — lists fittable coefficients with bounds
- `espressolab_cli synthesize --recipe <file> --coefficients <file> --noise <g> --out <file>` — generates a synthetic measured shot from the model's own output; always flags `"synthetic": true`
- `espressolab_cli calibrate --shots <dir> --coefficients <file> --fit <name,...> [--holdout <id-or-filename,...>] [--out <file>] [--report <file>]`
- `espressolab_cli calibrate ... --leave-one-out` — for 3-5 real shots from one fixed setup, cross-validates each fold instead of using one permanent holdout

## Fittable Coefficients

- Only names returned by `fit-params` can be moved — the guardrail against minimizing loss by turning every knob at once
- Start with `kozeny_constant` (overall resistance level) and `extraction_rate_ref_s` (extraction speed); add `initial_porosity` / `dry_permeability_multiplier` only if the flow-curve shape is still wrong after the level is right
- `kozeny_constant`, `extraction_rate_ref_s`, `flow_half_saturation_m3_s` are searched in log space (they span orders of magnitude)
- Optimizer is deterministic Nelder-Mead over the normalized parameter box: same shots, starting point, and parameter list always produce the same fitted file

## Synthetic-Recovery Sanity Check

- `./scripts/calibration_demo.sh` (builds Release if needed)
- Generates 3 synthetic shots (`baseline`, `pre-infusion`, `immediate-pressure`) from `assets/coefficients/default-v1.json` as ground truth
- Perturbs the starting point away from truth (`kozeny_constant` ×3, `extraction_rate_ref_s` ×0.4)
- Fits, holding out `immediate-pressure`, then checks each recovered coefficient is within **2%** of truth — prints `PASS`/`FAIL` per coefficient and exits nonzero on any failure
- This is CI's "Calibration recovery" step; it proves the fitter machinery works and says nothing about real espresso, since the "shots" are the model's own output
- Run after changing the loss function, coefficient bounds, `fit-params`, or the optimizer

## Leave-One-Out Acceptance Gate (real shots)

- Requires ≥3 real shots from one identically named machine setup, unique IDs, ≥2 time/mass samples per shot, a measured final shot time; rejects synthetic fixtures and mixed machine descriptions
- Passes only when median held-out mass RMSE ≤1g, median time error ≤2s, and no fold exceeds 2× either limit; if every shot has TDS, median TDS error ≤0.25pp and no fold >0.5pp; missing TDS is reported as "not assessed"
- A failed validation writes the report but never writes the requested coefficient file

## Provenance Rules

- Never edit a coefficient file in place — commit fitted output as a **new** file (e.g. `default-v2.json` or a machine-named id); every past run's result hash depends on the original file
- The `synthetic` flag must propagate into the calibration report and into `provenance.limitations` — never strip it

See `docs/calibration.md` for the full narrative workflow.

## Related Skills

- `build-and-test` for the `[calibration]`/`[recovery]` tags
- `data-contract-change` if the change touches `MeasuredShot`/`MeasuredSample`/`LossBreakdown` fields
- `pr-checklist` for stating whether evidence is synthetic-only vs. real-shot
