# Gaggia Classic Pro E24 synthetic reference shots

The three JSON files beside this note are synthetic calibration fixtures. They
are not observations from a physical machine and must never be presented as
real-shot validation. Each file keeps `"synthetic": true`, so
`calibrate --leave-one-out` correctly rejects it.

## Real hardware basis

The fixtures are scoped to the Gaggia Classic Pro E24 because Gaggia North
America publishes the relevant fixed hardware values: a 58 mm portafilter,
7-18 g basket capacity, and 9-bar OPV calibration. Source, accessed 2026-08-25:
<https://www.gaggia-na.com/products/gaggia-classic-pro>.

## Modeling choices

- Dose is 18 g, the listed basket maximum; target beverage mass is 36 g for a
  1:2 double. The yield target is a modeling choice, not a manufacturer claim.
- The direct 9-bar profile represents the machine's manual brew switch. No
  pre-infusion is assumed because this model does not identify a programmed
  pre-infusion system for this machine.
- The 93 C inlet profile, 9 mm puck depth, and 330/350/370 um particle diameters
  are explicit model inputs. Gaggia does not publish these values; they are
  plausible setup assumptions for comparing fine, baseline, and coarse grinds.
- The series are produced by EspressoLab's solver at five samples per second
  with 0.08 g Gaussian scale noise. Pressure comes from the imposed recipe
  profile. Neither series is a recorded machine trace.

Use these fixtures only to inspect import, fitting, and synthetic-provenance
behavior. Replace them with logged scale measurements before claiming that a
coefficient set was validated against the Gaggia or any other espresso machine.
