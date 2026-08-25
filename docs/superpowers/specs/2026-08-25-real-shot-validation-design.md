# Real-shot leave-one-out validation

## Scope

Calibrate one fixed espresso setup from three to five real measured-shot JSON
files. `--leave-one-out` fits only `kozeny_constant` and
`extraction_rate_ref_s`.

## Workflow

`espressolab_cli calibrate --leave-one-out` validates a dataset, fits one model
per held-out shot, aggregates held-out error, and then refits against every shot
only when the validation result passes. The coefficient artifact records the
complete leave-one-out evidence in its provenance.

The dataset must contain at least three non-synthetic files with unique IDs, a
single nonempty machine setup description, two or more time/mass points, and a
final shot time. TDS and pressure are optional. Pressure is reported only as a
diagnostic because the model receives the recipe pressure profile as input.

## Acceptance

Pass when median mass RMSE is at most 1 g and median time error is at most 2 s;
no fold can exceed twice either limit. When TDS is present for every shot, median
TDS error must be at most 0.25 percentage points and no fold can exceed 0.5.
Missing TDS is reported as not assessed. Failed validation emits the report but
does not emit a coefficient file.

## Verification

Tests cover fold isolation, deterministic aggregation, threshold failures,
optional TDS and pressure, and real-data dataset guards. Real validation remains
pending until the measured-shot JSON files are supplied.
