# Level 3 Axial Finite-Volume Cells Design

## Goal

Implement fidelity level 3: divide each lateral region into stacked axial cells
along the flow direction, so the puck resolves a real wetting front, a real
temperature profile, and a real solute concentration gradient from the screen
to the basket floor. This is a spatial discretization of the existing physics,
not a new momentum model.

## What this is not

This is not CFD. There is no momentum equation, no velocity field, no
Navier-Stokes, no turbulence model, and no resolved pore geometry. Flow remains
Darcy's law, now evaluated per cell with the cells in hydraulic series. Level 4
(CFD or particle-resolved) stays out of scope and out of the repository, and
nothing in this change should be described as CFD.

## Scope

- Add an optional `axial_cells` recipe field. Omitted resolves to `1`, which
  must reproduce the Level 2 aggregate results.
- Support 1 to 32 cells, applied uniformly to every lateral region, so a recipe
  is a `parallel_regions` x `axial_cells` grid.
- Keep the existing aggregate samples, summary, diagnostics, REST response, and
  CSV export as the dashboard contract.
- Report a per-cell final summary inside each region so the axial state is
  observable and testable.
- Do not add dashboard controls in this change.

## Model

Each cell owns puck temperature, liquid saturation, retained pore liquid,
dissolved solids in that pore liquid, and remaining extractable solids. A cell's
cross-sectional area is its region's area; its depth is the region's compressed
depth divided by the cell count; its dry coffee mass is the region's dose
divided by the cell count.

**Flow.** Cells stack in hydraulic series, so one volumetric flow passes through
the whole column and the resistances add:

```
R_region = sum_i ( mu_i * L_i / (k_i * A_region) )
Q_region = deltaP / R_region
```

Each cell's permeability uses the existing Level 1 relationship at that cell's
own saturation, so a dry cell below a wet one throttles the entire column. This
is what produces a physical pre-infusion: the front has to advance before the
column conducts. A cell with nonpositive permeability, depth, area, or viscosity
blocks its column and the region's flow is zero, matching the Level 1 guard.

**Transport.** Cell 0 receives inlet water at the inlet temperature carrying no
solids. Cell i receives the liquid leaving cell i-1, at cell i-1's temperature
and pore concentration. Liquid leaves a cell only once its pore volume is full,
exactly as at Level 2; what leaves the last cell is beverage.

**Heat.** Each cell is a lumped thermal mass exchanging with the liquid entering
it from upstream and with the environment, using the existing correlation with
the upstream temperature in place of the inlet temperature. The ambient loss
coefficient is divided across cells so a refined grid does not invent extra
loss.

**Extraction.** Evaluated per cell at that cell's temperature, saturation, and
flow, against that cell's remaining extractable solids. Dissolved solids leave
with the outgoing liquid at the cell's pore concentration, so lower cells
receive already-loaded liquid and extract into it. This is the mechanism the
lumped model could not express.

**Aggregation.** Aggregate beverage mass and cup solids are sums over regions.
Aggregate saturation is pore-capacity weighted over every cell, temperature is
thermal-capacity weighted, and permeability is area weighted over the
series-equivalent region permeability, so existing chart fields keep their
meaning.

## Compatibility And Invariants

`axial_cells = 1` must reproduce the Level 2 numbers within test tolerance, and
a recipe without the field stays valid. Adding the field changes the recipe hash
and therefore the result hash, and the physics changes for cell counts above
one, so the solver version moves to 0.4.0 and the documented baseline hash is
re-recorded.

Water and solids residuals become sums across every cell of every region and
must still close to within 1e-9 kg. Saturation stays within [0, 1] per cell. The
existing flow, temperature and saturation warnings keep firing once at first
occurrence.

## Files And Boundaries

- `include/espressolab/types.hpp`, `engine/espresso_core/types.cpp`: the field
  and its validation.
- `engine/model_library/puck.cpp`: series resistance, as a model-library
  function rather than arithmetic inside the solver.
- `engine/espresso_core/simulator.cpp`: per-cell state and the axial sweep.
- `include/espressolab/result.hpp`, `engine/artifact_io/json_io.cpp`: the
  per-cell summary.
- `schemas/recipe.schema.json`, `docs/model.md`, `docs/api.md`.

## Tests

- Validation rejects a cell count below one or above 32.
- One axial cell reproduces the Level 2 aggregate within tolerance.
- A refined grid converges: successive cell-count doublings move the shot time
  and yield by a shrinking amount.
- The wetting front is ordered: during pre-infusion an upper cell is strictly
  wetter than the cell below it.
- Lower cells see a higher inlet concentration than upper cells, so a cell's
  extraction falls with depth once the column is running.
- Water and solids balances close across the full cell grid.
- Repeated runs reproduce the same result hash.
- The per-cell summary reaches the serialized artifacts in order.

## Non-Goals

- No momentum, velocity field, or CFD of any kind.
- No lateral exchange between regions, and no radial structure inside a region.
- No dynamic channel formation.
- No dashboard controls for cell count in this change.
