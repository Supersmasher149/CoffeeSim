# Breville Barista Touch BES880 recipe configs

The three configs beside this note describe an 18 g double on the Breville
Barista Touch (BES880) at a fine, baseline, and coarse grind. They are brew
inputs for the solver, not measurements: no shot from this machine has been
logged, and nothing here validates a coefficient set against it.

## Manufacturer-published hardware

Breville publishes these fixed values for the BES880. Source, accessed
2026-08-25: <https://www.breville.com/en-us/product/bes880>.

- 54 mm stainless steel portafilter, with single and dual wall filter baskets.
- An "ideal dose of 18 g" for the double basket.
- A 15 bar pump regulated to a 9 bar extraction.
- Low pressure pre-infusion preceding the 9 bar extraction.

The machine holds brew temperature under PID control and exposes a temperature
adjustment, but Breville does not publish the setpoint or its range on the
product page, and the instruction manual was not reachable in a form this note
could cite. The 93 C inlet profile below is a model input, not a manufacturer
value.

## Modeling choices

Everything in this list is an EspressoLab input chosen to make the machine
comparable to the other configs. None of it is a Breville claim.

- **Puck depth, 10.4 mm.** Derived, not measured. This repository's 58 mm
  configs pack 18 g into a 9 mm bed, a packed density of 0.757 g/cm3. Holding
  that density in a 54 mm basket gives 23779 mm3 / 2290 mm2 = 10.38 mm. The
  narrower basket therefore produces a deeper bed at the same dose, which is
  most of why these shots run longer than the 58 mm Gaggia set.
- **Pre-infusion shape.** 2 bar held to 3 s, ramped to 9 bar by 6 s, then held.
  Breville states that pre-infusion is low pressure and automatic but publishes
  neither its pressure nor its duration, so the level follows this repository's
  existing low-pressure convention and the duration represents a brief soak.
  Treat both numbers as adjustable inputs.
- **Inlet temperature, 93 C**, flat, matching every other config here.
- **Particle diameters, 330 / 350 / 370 um**, deliberately the same triple as
  the Gaggia Classic Pro E24 configs, so that comparing the two machines
  isolates basket geometry and pre-infusion rather than grind. These are
  effective diameters, not grinder settings: the Barista Touch's integrated
  grinder has 30 dial positions and no published mapping to particle size
  (section 5.3).
- **Target beverage mass, 36 g** for a 1:2 double, with a 50 s time limit.

## What the solver currently returns

Run against `assets/coefficients/default-v1.json`, whose coefficients are
uncalibrated, so these are model outputs rather than predictions of what the
machine pours. Every run reaches target mass with no clamps.

| config | particle | shot time | TDS | yield |
| --- | --- | --- | --- | --- |
| fine | 330 um | 38.35 s | 10.26 % | 20.53 % |
| baseline | 350 um | 34.30 s | 9.77 % | 19.54 % |
| coarse | 370 um | 30.94 s | 9.27 % | 18.55 % |

Reproduce with:

```bash
espressolab_cli simulate \
  --recipe assets/recipes/breville-barista-touch-bes880-baseline.json \
  --coefficients assets/coefficients/default-v1.json
```

Replace this note's modeling choices with logged values before claiming that any
result describes a real Barista Touch shot.
