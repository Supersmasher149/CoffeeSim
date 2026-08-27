# Standard Simulator Core Refactor Design

## Goal

Make the standard Level 1-3 simulator easier to read and extend without
changing its numerical behavior, public API, result contracts, or ownership
model.

## Scope

This pass is limited to `engine/espresso_core/simulator.cpp` and focused tests
when needed. It does not refactor the experiment runner, calibration, CFD,
server, dashboard, artifact contracts, or model equations.

## Approved Approach

Keep `Simulator::run` as the only public orchestration entry point. Extract
meaningful private operations as functions in `simulator.cpp`; do not add public
classes, interfaces, factories, or a new runtime layer.

Retain the existing private value types:

- `Boundaries` for pressure and inlet-temperature boundary values
- `Derived` for geometry, flow, and water-property calculations
- `CellState` for mutable axial-cell state
- `RegionState` for mutable region state and integrated flow

Organize the implementation around these operations:

- `validate_inputs`
- `initialize_regions`
- `evaluate_regions`
- `advance_regions`
- `append_sample`
- `finalize_result`

The names describe domain operations rather than architectural layers. They
remain implementation details and pass state explicitly through references or
return values.

## Data Flow

The current calculation order remains unchanged:

1. Validate the recipe, coefficients, and timestep configuration.
2. Initialize parallel regions and their axial cells.
3. Evaluate boundaries, water properties, compressed geometry, cell
   permeability, pore capacity, and Darcy flow.
4. Update diagnostics, warnings, and the current sample.
5. Check numerical failure, target mass, and time-limit termination.
6. Copy the pre-step state and advance every region through its axial cells.
7. Interpolate samples crossed by the timestep.
8. Re-evaluate final derived values and assemble the result.

No global or hidden mutable state will be introduced. Warning recording stays
explicit through the existing result warning vector.

## Behavior Invariants

The refactor must preserve:

- `InvalidInputError` before any stepping for invalid inputs
- Existing numerical-failure and invalid-state termination behavior
- Warning codes, severities, timestamps, and once-only deduplication
- Timestep order, interpolation behavior, and sample timestamps
- Diagnostics, summaries, region summaries, and manifest fields
- Deterministic outputs and result hashes
- Level 2 arithmetic when `axial_cells == 1`

## Verification

Run the relevant simulator integration and unit tests before and after the
change. The regression set includes baseline shots, termination, invariants,
parallel regions, axial cells, determinism, and artifacts. Use representative
result hashes where available as an additional signal that behavior did not
drift.

## Out Of Scope

Further splitting into new translation units, changing public data structures,
introducing pause/resume support, changing formulas, changing numerical guards,
or redesigning ownership are deferred until a concrete need justifies them.
