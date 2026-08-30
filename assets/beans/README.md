# Bean profiles

A bean profile describes a coffee in the only terms the sensory overlay can act
on: how its extractable solids divide across six solute classes, and what the
roaster says the cup should taste like. See `docs/model.md`, "Sensory overlay".

A profile drives **no physical quantity**. Mass, flow, TDS and extraction yield
are bit-identical with and without one; the overlay partitions the solids the
solver already extracted rather than changing how much it extracts. Attaching a
bean to a recipe does change that recipe's hash, because it changes the flavour
the run reports and a run must record what it claimed.

## Every number in these files is an authored prior

None of it is measured. No solute-class share here came from a chromatogram, no
relative rate is a fitted rate constant, and no predicted axis has ever been
compared against a tasting panel. These files are not calibration data and must
never be turned into a calibration target — `espressolab_cli calibrate` reads
mass, time, TDS and pressure, and deliberately does not look at any of this.

What *is* grounded is the shape. Acids elute first and roast-degradation
bitterness and tannins last; a light roast retains more chlorogenic acids and
unreacted sugars while a dark roast converts sugars into melanoidins, raises
bitter degradation products, and migrates lipids to the bean surface. That
ordering is the standard qualitative account of roast chemistry. The specific
values are chosen to express it plausibly and to make the catalogue span a range
wide enough for differences between beans to be visible at all.

Two entries deserve naming individually:

- **`lipids` is the weakest class.** An emulsified phase is stripped
  mechanically, not dissolved, so a first-order rate ratio is arguably the wrong
  functional form for it. It is kept because body has to come from somewhere.
- **`bitter` is named for the sensation, not a chemical family.** Caffeine
  itself extracts fast, so calling the class "alkaloids" would imply a claim its
  slow rate contradicts. It is a lumped late-eluting bitter class.

## What a bean document contains

| Key | Meaning |
| --- | --- |
| `id`, `version` | Identity, versioned separately from the recipe and the solver |
| `classes.<name>.mass_fraction` | Share of extractable solids. The six must sum to 1 |
| `classes.<name>.relative_rate` | Extraction propensity relative to `maillard` (1.0). Optional; defaults to the shared model ladder |
| `axis_weights.<axis>.<class>` | How much each class feeds each sensory axis. Optional; defaults to the shared model matrix |
| `target.<axis>.intensity` | What the roaster says the coffee should deliver, 0-10 |
| `target.<axis>.tolerance` | Intensity points that count as one unit off. Optional, default 1.5 |
| `target.<axis>.weight` | Contribution to the match score. Optional, default 1.0; 0 excludes the axis |
| `description` | Roaster, origins, cupping notes, source, limitations. **Descriptive only** |

`relative_rate` and `axis_weights` are properties of the *model*, not of any one
coffee, which is why all three shipped beans share them and differ only in
`classes[].mass_fraction` and `target`. A document may override them.

`description` is deliberately excluded from `recipe_hash()` — the same rule, and
the same reason, as coefficient provenance. Fixing a typo in a tasting note must
not change what a run means or which directory its artifacts land in.

## The catalogue

| File | Roast | Stands for |
| --- | --- | --- |
| `counter-culture-hologram.json` | medium | Counter Culture's year-round washed/natural blend: fruit and milk chocolate together |
| `ethiopia-dhiba-bate-natural-light.json` | light | The natural Ethiopian component on its own, taken lighter — the fruit-forward end |
| `continental-dark-roast.json` | dark | A generic chocolate-forward dark blend — the opposite end |

Hologram is a rotating blend. Counter Culture changes its components through the
year while aiming at a consistent profile, so the 70 % Guatemala CODECH / 30 %
Ethiopia Dhiba Bate split recorded in that file is one published composition,
not a fixed recipe.
