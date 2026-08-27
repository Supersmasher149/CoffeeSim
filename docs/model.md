# The model

Every number the dashboard shows traces back to a named input, an equation on
this page, and a versioned coefficient in `assets/coefficients/`. Nothing is
tuned behind an unexplained constant.

## Fidelity

Level 3 of the ladder in section 4.1: one to eight lateral regions in hydraulic
parallel, each divided into 1 to 32 stacked axial finite-volume cells along the
flow direction. A recipe is therefore a `parallel_regions` x `axial_cells` grid.
Regions share the imposed pressure and inlet-temperature profiles; every cell
carries its own wetting, temperature, retained liquid, and extraction state.
Unequal permeability multipliers represent a fixed, first-order channel.

`axial_cells = 1` is the Level 2 lumped puck and reproduces it exactly, which is
the default for a recipe that does not set the field.

Level 4 exists as a **separate solver**, `CfdSolver`, described below. The
Level 1-3 pipeline above remains the default, and is what the CLI's `simulate`,
the REST server, and the dashboard use.

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

## Axial cells

Each region's compressed depth is divided evenly into `axial_cells` cells. A
cell has the region's cross-section and porosity and differs only by its state.

Cells stack in hydraulic series, so one volumetric flow passes through the whole
column and the resistances add:

```
R_region = sum_i ( mu_i * L_i / (k_i * A_region) )
Q_region = deltaP / R_region
```

Each cell's permeability uses the same relationship as Level 1 at that cell's
own saturation, so a dry cell below a wet one throttles the entire column. That
is what makes pre-infusion physical rather than a delay: the front has to
advance before the column conducts. Any cell failing the section 6.5 guards
blocks its column and the region's flow is zero.

Transport runs top to bottom in one sweep per step. Cell 0 receives inlet water
at the inlet temperature carrying no solids. Cell `i` receives the liquid that
left cell `i-1`, at that cell's temperature and pore concentration. Liquid
leaves a cell only once its pore volume is full, exactly as at Level 2, and what
leaves the last cell is beverage.

Heat and extraction are evaluated per cell with the upstream temperature in
place of the inlet temperature. The ambient loss coefficient is divided across
cells so that refining the grid does not invent extra heat loss.

The consequence worth naming: lower cells extract into liquid that is already
loaded, so the driving gradient and the local yield both fall with depth. A
lumped puck cannot express that, and it is why a resolved column reports a
higher aggregate yield than the same recipe at one cell.

Aggregate saturation is the pore-capacity weighted mean over every cell,
aggregate temperature is thermal-capacity weighted, and the reported
permeability is the series-equivalent value at the region's rolled-up
viscosity, so the existing chart fields keep their meaning.

## Parallel regions

Each region receives an `area_fraction` of the basket area and coffee dose. The
fractions sum to one. Its permeability uses the Level 1 relationship multiplied
by its configured `permeability_multiplier`:

```
k_region = k_eff * permeability_multiplier
Q_total  = sum(Q_region)
```

Every region sees the same pressure drop but its own local saturation and water
temperature. Pore filling, heat transfer, extraction, and output transport are
therefore evaluated per region. Aggregate beverage mass and cup solids are sums
of the regional values; aggregate TDS and extraction yield are calculated from
those totals. Reported aggregate temperature, saturation, and permeability are
thermal-capacity-, pore-capacity-, and area-weighted respectively.

## Flow

Darcy flow through a porous bed:

```
Q = (k * A / (mu * L)) * deltaP
```

with permeability from a Kozeny-Carman-shaped relationship:

```
k0        = d_p^2 * eps^3 / (C_k * (1 - eps)^2)
k_eff     = k0 * distribution_factor * wetting_factor
```

`C_k` in the default coefficient file is about 4.0e6, not the textbook 180 — see
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

- Momentum, a velocity field, or CFD of any kind. Flow is Darcy's law per cell.
- Lateral exchange between regions, radial structure inside a region, or
  dynamic channel formation.
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


## Level 4: the CFD solver

`CfdSolver` is a two-dimensional axisymmetric finite-volume solver for two-phase
flow through the puck. It is a separate entry point (`espressolab_cli cfd`), not
a replacement for the pipeline above: the deterministic Level 1-3 core, its
artifacts and its hashes are untouched by it.

### What it solves

```
total velocity      u_t   = -lambda_t grad(p),   lambda_t = lambda_w + lambda_a
pressure            div( lambda_t grad(p) ) = 0            (elliptic)
water saturation    phi dS/dt + div( f_w u_t ) = 0         (hyperbolic)
enthalpy, solute    advected on the water flux, with local source terms
```

`lambda_w = k k_rw(S) / mu_w` and `lambda_a = k k_ra(S) / mu_a`, with the
fractional flow `f_w = lambda_w / lambda_t`. The air phase is what lets a dry
cell fill: with a single incompressible phase the divergence-free field would
never accumulate anything.

The mesh is `radial_cells x axial_cells` over (r, z), with Dirichlet pressure at
the dispersion screen and the basket floor, no flow at the basket wall, and the
zero-area axis face as the symmetry condition. Transmissibilities use the
harmonic mean, which is the correct face average for resistances in series. The
pressure equation is solved by red-black SOR to a fixed relative residual; the
sweep order is fixed, so runs are reproducible. Saturation, enthalpy and solute
are advanced explicitly with donor-cell upwinding, and the face fluxes are
limited on the donor so that no cell exports more water or solute than it holds.
Because a limiter scales the shared face value, both cells see the same number
and the balances still close to machine precision.

### What it is not

The momentum closure is Darcy at the representative-elementary-volume scale.
There is **no pore-resolved Navier-Stokes**: the pore geometry is not meshed, so
inertia inside the pores is not resolved and no turbulence model applies. This
is the standard porous-media formulation, the same one reservoir and packed-bed
simulators use, and it is what "CFD" means for a medium whose pore structure is
represented statistically by a permeability. Calling it a pore-scale or
particle-resolved simulation would be false.

The current implementation uses a Darcy closure. It does not yet apply a
Forchheimer inertial correction, so callers must not interpret the configuration
placeholder for that term as an implemented model feature.

It is also **not validated**. Verification and validation are different claims:
the tests below check the solver against the equations it says it solves, and
nothing here checks those equations against a real shot. The coefficients are
the same uncalibrated set the rest of the project uses.

### Verification

`[cfd][verification]` covers, at every mesh size it runs:

| Check | Result |
| --- | --- |
| Discrete `div(u_t)` | ~1e-8 to 1e-7 1/s, against Darcy velocities of ~1e-4 m/s |
| Water mass residual | ~1e-17 kg |
| Solute mass residual | ~1e-18 kg |
| Axisymmetry of a uniform bed | radial variation below 1e-7 relative |
| Isothermal pressure field | linear in depth to 5e-4, shrinking under refinement |
| Darcy velocity vs analytic | within 1 % |
| Saturation bounds | `0 <= S <= 1`, zero clamps in the standard verification configurations |
| Mesh refinement | yield gaps shrink monotonically |
| Determinism | identical fields across runs |

The isothermal case is a method-of-exact-solutions check: uniform mobility
reduces the pressure equation to `d2p/dz2 = 0`, whose solution is linear. The
residual 5e-4 departure is the saturation profile, identified as such because it
falls as the mesh is refined.

### What it resolves that Level 3 cannot

A radial coordinate. A channelled recipe at Level 2 or 3 is *told* how the flow
splits between regions; the CFD solver is told only where the permeability
differs, and resolves the resulting radial pressure gradient and cross-flow
itself.
