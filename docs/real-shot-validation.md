# Real-shot validation

Nothing in EspressoLab has been checked against a real espresso shot. The
default coefficients are a plausible uncalibrated baseline, the measured-shot
fixtures in `assets/measured_shots/` are solver output plus Gaussian noise, and
`assets/reference_shots/` holds published *recipes*, not measurements. That gap
is what this document addresses: where trustworthy real-shot data can come
from, and a repeatable protocol for producing it.

This document covers acquisition and validation. Fitting is separate and stays
in [calibration.md](calibration.md) — see "Calibration is not validation" below.

## Researched sources

| Source | Contains | Lacks | Access / license | Parser testing | Chemistry reference | Calibration | Held-out validation |
| --- | --- | --- | --- | --- | --- | --- | --- |
| [Visualizer](https://visualizer.coffee/) shot API | Per-shot JSON: `timeframe` plus `data.espresso_pressure`, `espresso_flow`, `espresso_weight`, `espresso_flow_weight`, `espresso_temperature_basket`, `espresso_temperature_mix`, `espresso_resistance`, and the `*_goal` setpoint series; `bean_weight`, `drink_weight`, `drink_tds`, `drink_ey`, `grinder_model`, `grinder_setting`, `roast_date`, `roast_level`, free-text notes | **No machine model field**, no basket geometry, no puck depth, no PSD, no water recipe, no dose/yield scale resolution, no TDS method, no timing convention, no statement of whether the pressure series is pump or puck | Public shots readable anonymously (`GET /api/shots/<uuid>`, 200 requests / 10 min per IP). Shot data is user-submitted with **no stated reuse license** | **Yes — best available.** Real telemetry in a stable JSON shape | No | **No.** Heterogeneous across thousands of machines, baristas and unverified refractometers | Only after a specific shot's gaps are closed by contacting its author |
| Visualizer [public compare example](https://visualizer.coffee/shots/7320f3a9-e2f6-4627-b745-b62c6d5f4d0d/compare/dbcaaa51-e830-4d54-9ae9-1494f78b6b8b) | Two shots, same coffee/grinder/setting: 20.1 g → 47.7 g in 72.8 s, TDS 10.94 %, EY 26.0 %; and 20.3 g → 47.7 g in 79.5 s, TDS 10.62 %, EY 25.0 %. 351-sample telemetry each | Same gaps as above. The web page's machine attribution is derived from free-text notes, not a field | Public, no stated license | Yes — the fixture this repo probed | Weak (n=2, one barista) | No | No |
| [Coffee Ad Astra](https://www.patreon.com/coffeeadastra/posts/comparison-and-49666680) comparison post (Patreon) | Careful single-author extraction experiments, well-reasoned methodology | Post body is **not publicly retrievable** (HTTP 403 without a Patreon session); no machine-readable dataset is offered | Patron-only; all rights reserved | No | Background reading only | No | No |
| [Coffee Ad Astra — EG-1 vs Niche blooming espresso](https://coffeeadastra.com/2021/05/10/a-comparison-between-standard-and-low-fines-espresso-shots/) (public, freely readable — a *different* post from the Patreon-locked one above) | Jonathan Gagné, 2021-03-02: EG-1 (SSP Ultra-Low-Fines) vs Niche Zero, Decent DE1+, Scott Rao blooming profile. Shot-level dose/yield/TDS(raw+filtered)/EY/peak-pressure published in a public Google Sheet, plus a zipped Dropbox archive of the author's 11 raw DE1 `.shot` telemetry files (pressure, flow, cumulative weight, mix temperature; ~380 samples/shot) | No basket diameter, no puck depth, no PSD (grind is a dial number), no measured brew temperature — still not simulable without inventing inputs | Public; the shot-files archive states no explicit license | **Yes** — `tools/import_shot_telemetry.py`, run against all four shots this repo carries | No | No — see `espresso_real_world_refs/` below | No |
| [TUM / Mendeley dataset](https://data.mendeley.com/datasets/y2tz67f6ry/1) — Pannusch, Schmieder, Vannieuwenhuyse, Minceva, Briesen (TU München, 2023), DOI 10.17632/y2tz67f6ry.1 | MATLAB parameter-estimation code and an "Espresso Brewing Control App" for the kinetics model behind the Foods paper | It is **code, not the raw extraction data**; no shot telemetry | **CC BY-NC 3.0** — non-commercial only. Compatible with this repository only as a cited reference, not as vendored data | No | **Yes** — kinetic model structure for extraction of TDS and named compounds | No | No |
| [Schmieder et al., *Foods* **12**(15):2871 (2023)](https://doi.org/10.3390/foods12152871), "Influence of Flow Rate, Particle Size, and Temperature on Espresso Extraction Kinetics" | Decent DE1 Pro (flow-controlled) + Mahlkönig E65S at three settings; laser-diffraction PSD (De Brouckere means 273–295 µm, Sauter means 26.9–29.2 µm); TDS by DR6000-T refractometer per DIN 10775; flow 1/2/3 ml·s⁻¹ × 80/89/98 °C central composite design; 20.00 ± 0.01 g dose; ten sequential fractions per extraction; 48 extractions | Per-shot telemetry time series are not published. **Data availability is "available from the corresponding author upon request"** — nothing downloadable | CC BY (gold open access) | No | **Yes — the strongest chemistry/extraction reference found.** Directly relevant to the extraction-rate and grind-factor correlations in [model.md](model.md) | Not as-is; the fractionated protocol is not a normal shot. Could become a calibration target only if the authors supply the raw data | No |

### What this means

- **No public source is ready to calibrate against.** Every candidate is missing
  at least one input the solver requires: basket geometry, tamped puck depth, a
  measured particle size distribution, or a measured brew-water temperature.
- **Visualizer is the right target for import/parser work** and nothing more.
  Its records are real, but a scalar dial setting and a machine-attributed
  pressure series of unknown location cannot be turned into model inputs without
  inventing numbers.
- **The Foods paper is the right extraction-kinetics reference** and is properly
  licensed to cite. The Mendeley companion is CC BY-NC, so its contents must not
  be vendored into this repository.
- **The strongest validation set has to be produced here**: several controlled
  shots on one documented machine with the same coffee, water, basket and
  preparation, varying one factor at a time. That is what the protocol below is
  for.
- **Real telemetry from a public source did materialize**, but not from
  Visualizer — see "Real-world reference telemetry" below. It is shape-
  comparison and provenance, still not a validation set: the same missing
  inputs (basket geometry, puck depth, PSD, measured temperature) apply.

## Real-world reference telemetry

`espresso_real_world_refs/` (served by the REST server via `--references`,
rendered by `web/src/features/references/ReferenceShotsPanel.tsx`) carries
four shots from the Coffee Ad Astra EG-1-vs-Niche post in the table above:
`real_gagne_eg1_01`, `real_gagne_eg1_07`, `real_gagne_niche_02`,
`real_gagne_niche_06`. Each record's shot-level metadata (dose, TDS raw and
filtered, extraction yield, peak pressure, puck prep, ...) was already
transcribed from the article and its Google Sheet log. `timeseries` and
`observed.final_shot_time_s` were the acknowledged gap — the manifest
originally stated plainly that no telemetry had been fabricated and both were
empty/null until the original DE1 `.shot` files were parsed.

**That gap is now closed for these four shots.** The article links a zipped
Dropbox archive of the author's raw DE1 shot exports
(`shotfiles_Mar2_2021.zip`); the four filenames each record's
`source.de1_shot_file` already names (`20210302T094131.shot` and its three
siblings) are in that archive. `tools/import_shot_telemetry.py` parses the
DE1 `.shot` format (`espresso_elapsed`/`espresso_pressure`/`espresso_flow`/
`espresso_weight`/`espresso_temperature_mix`) and populates `timeseries`
(rows in `timeseries_fields` order: `time_s`, `pressure_bar`, `flow_ml_s`,
`beverage_mass_g`, `temperature_c`) and `observed.final_shot_time_s`. Each
record's `source.data_quality` says so and names the file. The raw `.shot`
exports themselves are **not** vendored into this repository — the archive
states no explicit license, so only these derived numeric samples are
reproduced, consistent with the shot-level metadata already committed here;
see `tools/import_shot_telemetry.py`'s module docstring for the exact
provenance and how to regenerate from a freshly downloaded copy of the
archive.

This remains **not a validation set**: `simulation_link`-equivalent inputs
(basket diameter, puck depth, PSD, a measured brew temperature) are still
absent, so these four shots cannot be simulated without inventing numbers.
They are real curves for shape comparison against the dashboard's own shot,
with honest, traceable provenance — nothing here is calibrated or validated
against them.

## Capture protocol 1.0

One shot produces two files in `assets/real_shots/`:

| File | Purpose |
| --- | --- |
| `<id>.capture.json` | Everything about the shot: setup, provenance, observations, and what was *not* measured |
| `<id>.telemetry.csv` | The time series, columns exactly `time_s,pressure_bar,flow_ml_s,cumulative_beverage_mass_g,temperature_c` |

Start from `TEMPLATE.capture.json` and `TEMPLATE.telemetry.csv`. The document
format is specified by [`schemas/real-shot-capture.schema.json`](../schemas/real-shot-capture.schema.json)
and enforced by `tests/schemas/real_shot_capture_check.py`.

### Before the session

1. Fix one machine, one coffee, one water recipe, one basket, one preparation
   routine. Record them once; vary **only** grind across the session.
2. Brew at least five shots. Reserve at least one, chosen before any fitting,
   as a held-out validation shot.
3. Decide the timing convention (`pump_on` or `first_drip`) and use it for both
   the record and the CSV. Comparing a pump-on shot time against a first-drip
   one is the easiest way to invent a two-second error.
4. Establish what the machine's logged pressure actually measures — pump, group,
   or puck. If you cannot establish it, record `unknown`. `unknown` is an
   observation; a guess is not.

### Per shot

1. Weigh the dose and record the scale's resolution.
2. Record the tamped bed depth (`preparation.puck_depth_mm`). No consumer
   machine reports it and the solver needs it.
3. Log telemetry if the machine can; otherwise leave `telemetry.csv` null. A
   scale-only cumulative-mass trace is still worth having.
4. Record `timing.first_drip_s`, `timing.end_condition` and
   `timing.total_shot_time_s` against the chosen convention.
5. Weigh the beverage; record the scale resolution.
6. Measure TDS: let the sample cool to a stated temperature, syringe-filter it,
   and take **repeated readings, all of them recorded** in
   `result.tds_method.readings_percent`.
7. Note taste, channelling, puck condition and any anomaly. Taste is logged, but
   it is never a calibration target.

### Rules the contract test enforces

- Anything not measured is `null`. Never `""`, `"TBD"`, `"n/a"`, and never an
  estimate.
- A grinder dial setting is stored as **text** and is never converted to microns.
  PSD numbers require `grind.measured: true` and a stated `grind.method`.
- `temperature.setpoint_c` is what the machine was told to do;
  `temperature.measured_c` requires a measurement point and method.
- `machine.pressure_sensor_measures` is mandatory whenever telemetry exists.
- A `computed` extraction yield must equal `tds × yield ÷ dose` to within
  0.05 percentage points.
- `telemetry.time_origin` must equal `timing.convention`.
- If `simulation_link.unavailable_model_inputs` is non-empty,
  `simulation_link.recipe` stays null: the shot is not simulable without
  inventing a number.
- A `pending_capture` record carries no observations at all.

## Comparing a capture against a simulation

Only the entries listed in `simulation_link.comparable_observables` may be
scored, and only against a simulation whose inputs came from the same record in
the same units.

| Scalar | Report |
| --- | --- |
| `beverage_yield_g`, `total_shot_time_s`, `first_drip_s`, `tds_percent`, `extraction_yield_percent` | Absolute error, in the observable's own unit |

| Curve | Report |
| --- | --- |
| pressure, flow, temperature, cumulative mass | MAE and RMSE — **only** when the measured and simulated series share a time origin. If they do not, resample or report nothing |

Two traps worth naming, because the model invites both:

- Pressure is a recipe *input* to this solver, not a prediction. A pressure RMSE
  is a diagnostic that the machine followed its profile, not a model score.
- The solver's sampled flow is Darcy flow *into the bed*; a scale measures mass
  *out of the spout*. They differ until the pores fill. Compare cumulative mass,
  or accept the offset explicitly.

## Calibration is not validation

- Validation scores a fixed coefficient set against shots. Calibration changes
  the coefficients. Never report a fit's own loss as validation.
- Fit a small, physically interpretable coefficient set across several shots —
  not one shot exactly. `--leave-one-out` exists for exactly this
  (`assets/measured_shots/README.md`).
- At least one shot is held out and never fitted on, and is named in the
  coefficient file's `provenance`.
- Any coefficient set fitted to real shots must record dataset identity, fit
  date, machine, known limitations and provenance, so a later re-fit never
  silently changes what a past run meant.

## Promoting a capture to a measured shot

`assets/real_shots/` is inert: nothing in the build reads it, and
`espressolab_cli calibrate` never sees it. A capture becomes usable by the
fitter only when every solver input is present and non-null — dose, basket
diameter, puck depth, an effective particle size or PSD, and the brew
temperature — at which point it is transcribed into a measured-shot document in
`assets/measured_shots/` with `synthetic: false`. The capture record stays
behind as its provenance.

`espressolab_cli calibrate` reads **every** `.json` in its shots directory as a
measured shot, which is why capture records live in their own directory and use
the `.capture.json` suffix. The contract test fails if one appears in
`assets/measured_shots/`.
