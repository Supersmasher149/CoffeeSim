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

Level 4 exists as a **separate solver**, `CfdSolver`, described below. Level 4b
is the separate Cartesian solver, `Cfd3dSolver`, described in the final section.
The Level 1-3 pipeline above remains the default, and is what the CLI's
`simulate`, the standard shot REST endpoint, and the dashboard use.

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

### Particle size distribution

A recipe may supply `puck.grind` — a set of `(diameter_um, mass_fraction)`
bins — instead of the scalar `particle_diameter_um` / `particle_spread_factor`
pair. The two spellings are mutually exclusive and the loader rejects a document
carrying both.

When a distribution is present, `d_p` above is its **Sauter mean diameter**:

```
d32 = 1 / sum(w_i / d_i)
```

This is not a new model. Kozeny-Carman's length scale *is* the bed's
surface-area-to-volume ratio, and d32 is by definition the diameter of the
monodisperse bed with the same ratio — so the polydisperse form of the equation
already in use is exactly `d_p := d32`. A one-bin distribution returns its own
diameter, and reproduces the scalar shot exactly.

The spread penalty is likewise derived rather than authored:

```
sigma_g = exp(sqrt(sum(w_i * (ln d_i - mean_ln_d)^2)))
spread  = clamp(ln(sigma_g) / ln(4), 0.1, 1.0)
```

`sigma_g` is 1.0 for a monodisperse grind and grows with polydispersity. The
reference `4` is a **fixed model choice, deliberately not a coefficient**: every
member of `ModelCoefficients` is hashed into `coefficient_hash()` and therefore
into every `result_hash`, so adding one would rewrite the hash of every existing
run for a code path those runs never take. It is chosen so a typical espresso
grind (`sigma_g` ≈ 2.2) lands near 0.57 — within rounding of the 0.55 this
project has defaulted to since the scalar-only model — meaning converting a
recipe to a distribution does not silently step its permeability.

Extraction becomes size-resolved: each bin carries its own extractable pool and
its own rate, since `grind_factor` is already `(d_ref/d)^n` and is simply
evaluated per bin. With `grind_exponent = 1` the mass-weighted mean rate equals
the rate at d32 exactly (because `sum(w_i/d_i)` *is* `1/d32`), so a distribution
and a lumped puck at its own d32 start out identical and diverge only as the
fast bins empty. That divergence — fines exhausting early while the coarse mode
keeps producing — is the behaviour a single diameter structurally cannot
produce, and it is what carrying a distribution buys.

Bin diameters are allowed over 10–2000 µm, far wider than the scalar envelope,
because real coffee fines sit at 10–100 µm. It is the *derived* d32 that must
land in the supported 150–800 µm band, since that is the range the correlations
were shaped around.

**Not validated.** The distribution changes what the model can represent, not
what it has been checked against. No PSD here has been compared to a measured
shot, and the default coefficients remain the same uncalibrated set.

### The grinder (`espressolab_cli grind`)

A separate comminution model produces such a distribution from burr geometry.
It sits outside the shot pipeline in the same position as the CFD solvers: it
reads no recipe and no `ModelCoefficients`, writes its own artifacts, and cannot
affect a shot's result hash. Its output is a `GrindDistribution` that a recipe
may then carry.

A standard population balance over a fixed logarithmic size grid. On each pass a
mass fraction `S(d)` of every class breaks and is redistributed over the smaller
classes by `B`:

```
selection   S(d)       = 0                                     for d <= gap
                       = clamp(S_rate * (d/gap - 1)^alpha, 0, 1) otherwise
breakage    B(d_i|d_j) = (d_i / d_j)^beta        (Broadbent-Callcott, normalised)
fines       a fraction phi of every broken parent bypasses B and goes to the
            cell-wall mode at its own characteristic size
```

Both closures are textbook comminution. Two details carry the model's weight:

- **The gap classifies.** A particle at or below the burr gap has left the
  grinding zone and is finished product, so it is never selected again. Without
  that cutoff every class grinds down toward the fines mode and the model
  converges on a distribution far finer than any real grinder produces — with
  it, the coarse mode sits at the gap, as it should.
- **The fines term is per event, not per unit feed.** Mass is broken repeatedly
  on its way down from whole beans, so a `fines_yield` of 0.01 accumulates to
  roughly 3–4% of total mass. That is the order real grinds show: fines dominate
  by particle count, not by mass.

At the shipped defaults the mapping is roughly `d32 ≈ 0.4 * burr_gap`, rising
monotonically with the gap. Mass is conserved to machine epsilon across every
pass, and the model is fully deterministic — no RNG, fixed evaluation order.

**Not validated, and not a dial model.** `burr_gap_um` is a physical length;
nothing maps a grinder dial number onto it. The coefficients are a plausible
baseline in exactly the sense the shot model's defaults are, and no distribution
this produces has been compared against a measured one. A grinder spec is also
free to describe a bed the shot correlations do not cover — a fine enough gap
yields a d32 below the supported 150–800 µm band, and a recipe carrying it is
rejected. The CLI says so rather than letting it fail later.

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

## Level 4b: Cartesian 3D CFD

`Cfd3dSolver` is a separate structured Cartesian solver over `(x, y, z)` plus
time. It is invoked explicitly by `espressolab_cli cfd3d` or the 3D REST
endpoints. It does not replace the Level 1-3 solver, change standard shot
artifacts, or change standard result hashes.

The circular puck is embedded in an `nx x ny x nz` cell-centred grid. Each
`x/y` cell stores its analytic circle intersection area and each internal face
stores its analytic circle-line aperture. Cells with less than five percent of a
full Cartesian cell are deterministically agglomerated into a neighbouring
active cell. The geometry uses a relative tolerance of `1e-12`; all fields are
stored x-fastest, then y, then z.

For each axial column, the solver applies Darcy mobility using the existing
water and dry-phase closures. The pressure equation is assembled from aperture
and harmonic transmissibility coefficients and solved with a bounded
deterministic PCG iteration. A uniform-permeability XY state uses an exact
column reduction of that same discrete system; heterogeneous material fields
use the full Cartesian system. Saturation, temperature, pore TDS, extraction,
and velocity fields are advanced at a fixed timestep with the same guardrails
and mass-balance accounting as the native model.

The v1 3D material input is a cell-centred permeability multiplier in `[0.05,
20]`. It is deliberately not a new equation or a dynamic channel model. The
solver reports seven optional snapshot fields: pressure, saturation,
temperature, pore TDS, and the three velocity components. Snapshot emission is
bounded to 128 snapshots and 1 GiB of float64 field data.

This remains an engineering REV-scale Darcy model, not pore-resolved CFD. The
coefficients are the same uncalibrated defaults used elsewhere in the project,
so the current verification establishes geometry, determinism, invariants and
convergence properties rather than validation against measured shots.
