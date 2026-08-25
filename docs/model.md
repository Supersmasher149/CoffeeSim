# The model

Every number the dashboard shows traces back to a named input, an equation on
this page, and a versioned coefficient in `assets/coefficients/`. Nothing is
tuned behind an unexplained constant.

## Fidelity

Level 1 of the ladder in section 4.1: a one-dimensional lumped puck with
time-varying flow and extraction. Not level 2 (parallel regions and
channelling), not level 3 (axial finite-volume cells), and explicitly not level
4 (CFD or particle-resolved).

## State

```
time_s, puck_temperature_k, permeability_m2, liquid_saturation,
remaining_extractable_solids_kg, dissolved_solids_kg, beverage_mass_kg,
cumulative_water_in_kg
```

Two bookkeeping members are carried alongside the documented vector —
`retained_water_kg` (the pore liquid, water plus dissolved solids) and
`dissolved_solids_in_cup_kg` — so the mass balances close without re-deriving
them from the output series.

## Flow

Darcy flow through a porous bed:

```
Q = (k * A / (mu * L)) * deltaP
```

with permeability from a Kozeny-Carman-shaped relationship:

```
k0        = d_p^2 * eps^3 / (C_k * (1 - eps)^2)
k_eff     = k0 * distribution_factor * wetting_factor * channel_factor
```

`channel_factor` is exactly 1.0 in the MVP uniform-puck model. `C_k` in the
default coefficient file is about 4.0e6, not the textbook 180 — see
`assets/coefficients/default-v1.json`, which states why in full. Short version:
a single representative particle diameter cannot express a real fines-filled,
tamped bed, and `C_k` is where that discrepancy is parked until measured shots
replace it.

Compression and porosity respond to pressure through a bounded empirical curve:

```
compression = clamp(c_p * log1p(deltaP / P_ref), 0, compression_max)
L           = L0 * (1 - compression)
eps         = clamp(eps0 * (1 - c_e * compression), eps_min, eps0)
```

Guards (section 6.5): a negative pressure difference gives zero flow;
nonpositive viscosity, depth, area or permeability return zero rather than
dividing; the maximum-flow clamp is a numerical guard that always raises a hard
warning when it fires.

## Wetting

Pore liquid accumulates before anything leaves the puck. Liquid exits only once
the pore volume is full, which is what delays beverage production during
pre-infusion:

```
pore_capacity  = A * L * eps * rho(T_puck)
out            = max(pore_liquid - pore_capacity, 0)
saturation     = pore_liquid / pore_capacity
wetting_factor = dry_multiplier + (1 - dry_multiplier) * smoothstep(S)
```

## Temperature

One thermal mass exchanging heat with the incoming water and the environment:

```
C_total * dT_puck/dt = m_dot * c_p_water * (T_in - T_puck) - h_loss * (T_puck - T_ambient)
C_total              = dose * c_p_coffee + retained_liquid * c_p_water
```

Extraction kinetics are evaluated at the **puck** temperature, never the inlet
temperature. Water density, viscosity and heat capacity come from a small
validated interpolation table over 0-100 °C (`TabulatedWaterProperties`), chosen
over an embedded correlation because it is easier to test and to explain.

## Extraction

Bounded first-order transfer into the pore liquid:

```
dM_extracted/dt = k_ext * M_available
k_ext = k_ref * temperature_factor * grind_factor * saturation_factor * flow_contact_factor
```

| Factor | Form | Guardrail |
| --- | --- | --- |
| Temperature | `exp(-Ea/R * (1/T - 1/T_ref))` | exponent clamped to ±10 |
| Grind | `(d_ref / d_p)^alpha` | clamped to [0.05, 20]; zero for `d_p <= 0` |
| Saturation | `smoothstep(S)` | exactly zero at a dry puck |
| Flow contact | `Q / (Q + Q_half)` | strictly below 1 |
| Availability | remaining extractable solids | never negative |

Dissolved solids leave with the outgoing liquid at the current pore
concentration, so the cup fills more slowly than the puck extracts:

```
C_pore            = dissolved_in_pore / pore_liquid
solids_out_rate   = beverage_mass_flow * C_pore
```

Reported extraction yield uses the solids that reached the cup, which is what a
refractometer measurement can actually be compared against.

## Metrics

```
extraction_yield = dissolved_solids_in_cup / dry_coffee_mass
TDS              = dissolved_solids_in_cup / beverage_mass
brew_ratio       = beverage_mass / dry_coffee_mass
```

## What this does not model

- Channelling, uneven distribution, or any spatial structure in the puck.
- Grinder dial settings. Particle diameter is a physical input; there is no
  universal mapping from a number on a grinder.
- Flavour. Estimated TDS and extraction yield are engineering outputs. Taste
  depends on compound composition, roast, water chemistry, distribution and
  sensory context that this model does not resolve.
- Crema, degassing, pressure dynamics inside the group head, or pump behaviour.

## Numerics

Fixed 0.01 s explicit step, sampled every 0.05 s. The update order is fixed
(section 9.2): boundaries → water properties → geometry and permeability → flow
and saturation → heat → extraction and transport → advance → termination check.

Invariants are checked every step: all state finite, saturation within [0, 1]
plus tolerance, masses nonnegative, extracted solids bounded by the initial
extractable mass. A material violation terminates the run with a recorded reason
rather than producing a plausible-looking number.

Both mass balances are reported as residuals in the diagnostics, and in practice
close to around 1e-17 kg on the baseline recipe. `tests/integration/test_convergence.cpp`
runs the same shot at 0.02, 0.01 and 0.005 s and requires the change to shrink
as the step halves.
