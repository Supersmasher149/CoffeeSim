# Level 2 Parallel Regions Design

## Goal

Implement fidelity level 2: lateral puck regions that share the imposed pressure
profile but evolve their own hydraulic, wetting, thermal, and extraction state.
The feature models the first-order effect of channelling without adding axial
cells, CFD, or dashboard controls.

## Scope

- Use the existing optional `parallel_regions` recipe field. Omitted regions
  resolve to one uniform region with area fraction and permeability multiplier
  of `1.0`.
- Support one to eight regions. Area fractions sum to one; each permeability
  multiplier remains within the current validated range.
- Keep the existing aggregate samples, summary, diagnostics, REST response, and
  CSV export as the dashboard contract.
- Populate the existing final `regions` result array for every shot.
- Add a documented channelled recipe and update model/API documentation.
- Do not add dashboard controls or regional visualizations in this change.

## Model

Each solver region owns puck temperature, effective permeability, liquid
saturation, remaining extractable solids, dissolved solids in its pore liquid,
retained liquid, solids delivered to the cup, cumulative incoming water, and
cumulative flow volume.

All regions sample the same pressure and inlet-temperature profiles at the same
solver timestamp. A region's cross-sectional area and dry coffee mass are the
recipe basket area and dose multiplied by its `area_fraction`. Its effective
permeability is the existing Level 1 permeability relationship multiplied by
its `permeability_multiplier`.

For each explicit solver step, every region independently evaluates geometry,
permeability, Darcy flow, pore capacity, wetting, heat transfer, extraction,
and pore-liquid transport. The region calculations use their local state and
area but the shared pressure drop and inlet temperature. The shot terminates
against total beverage mass or the existing time limit.

The aggregate output is the sum of regional flow, water input, beverage mass,
and cup solids. Aggregate TDS and extraction yield use total cup solids over
total beverage mass and total dose. Aggregate puck temperature, saturation,
and permeability are respectively thermal-capacity-weighted,
pore-capacity-weighted, and area-weighted values so existing chart fields stay
meaningful.

`RegionSummary` reports the configured area fraction and permeability
multiplier, regional beverage mass, the region's share of integrated shot flow,
regional TDS, and regional extraction yield against its allocated dry coffee
mass.

## Compatibility And Invariants

A single default region must reproduce the Level 1 solver's numerical results
within test tolerance. Recipes and endpoints without `parallel_regions` stay
valid and resolve to that uniform configuration.

The water and solids residuals become sums across all regional balances. The
solver checks finiteness and saturation bounds for every region. Existing flow
and temperature guard warnings remain, with each condition emitted once at its
first occurrence. A regional maximum-flow clamp makes the aggregate output a
guard value just as in Level 1.

The result hash continues to include the recipe, all aggregate samples, and the
ordered final regional summaries. The solver version changes because output
semantics and physics change.

## Files And Boundaries

- `engine/espresso_core/simulator.cpp`: implement local regional state and
  aggregation inside the solver.
- `include/espressolab/result.hpp` and artifact serialization: retain the
  existing public result contract and populate regional data.
- `assets/recipes/`: add a two-region channelling example.
- `docs/model.md` and `docs/api.md`: document the Level 2 equations, fields,
  and limits.
- `tests/`: cover the model and artifact behavior; no React work is included.

No HTTP, artifact, or frontend layer calculates physics. They serialize or
render solver output only.

## Tests

- Recipe validation rejects invalid regional counts, fractions, totals, and
  permeability multipliers.
- One default region matches baseline aggregate outputs within numerical
  tolerance.
- An asymmetric two-region recipe gives the higher-permeability region a larger
  integrated flow fraction and distinct regional output.
- Regional and aggregate water and solids balances remain within the established
  tolerance.
- Repeated Level 2 runs have the same result hash.
- Level 2 results converge as the fixed time step is halved.
- JSON, summary, result, and CSV artifact contracts include stable regional
  output where applicable.

## Non-Goals

- No spatial coupling between regions, lateral migration, or a dynamic channel
  formation model.
- No axial finite-volume cells, pump or group-head dynamics, CFD, or flavour
  prediction.
- No dashboard region editor, chart overlays, or regional visualization.
