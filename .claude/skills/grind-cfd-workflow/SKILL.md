---
name: grind-cfd-workflow
description: Explains the grind/comminution model and the two CFD solvers (axisymmetric and 3D Cartesian) — deliberately separate Level 4 systems outside the default shot pipeline, each with its own CLI command, IO, and artifacts. Use when touching engine/grind/, engine/cfd/, engine/cfd3d/, the puck.grind particle distribution, espressolab_cli grind/cfd/cfd3d, or anything that risks coupling them to the default simulate pipeline's result hash.
---

# Grind and CFD Workflow

## The Rule

- `engine/grind/`, `engine/cfd/`, `engine/cfd3d/` must never become a hidden dependency of the default Level 1-3 pipeline (`espressolab_cli simulate`) and must never change its artifacts or result hashes
- Per CLAUDE.md's ownership table: `espressolab_grind` must not own recipes, shot artifacts, the default pipeline, or any result hash; `espressolab_cfd`/`espressolab_cfd3d` must not own REST, dashboard, standard artifacts, or default result hashes

## Grind / Comminution Model

- `espressolab_cli grind [--spec <file>] [--out <dir>]` — burr geometry → particle size distribution, lives in `engine/grind/`
- Own documents, own schemas (`schemas/grinder-spec.schema.json`, `schemas/grinder-result.schema.json`), own loader/serializer (`engine/grind/grinder_io.cpp`)
- **Not** shot artifacts: no recipe hash, no coefficient hash, no result hash
- Every spec field is optional and defaults to the compiled-in `GrinderSpec`, so `{}` is a valid spec
- The result's `distribution` object matches `recipe.puck.grind`'s shape exactly, so it pastes across unchanged; `grind --out` also writes `recipe-grind.json` for this purpose
- Bin diameters emitted at nanometre resolution (same fixed-point rule as recipe grind bins)
- A valid spec can still produce a distribution a recipe rejects (derived d32 must land in 150-800 µm) — that's intended, not a bug, and the CLI reports it directly
- Nothing here is validated against a measured grind; the output's `provenance` field carries that caveat with the document itself

## The Two Grind Spellings (recipe side)

- A recipe carries **either** the scalar pair `particle_diameter_um` + `particle_spread_factor` **or** the distribution `puck.grind.bins` — never both (`CONFLICTING_FIELD` error if both present)
- When `puck.grind` is present, the loader derives `particle_diameter_m` (d32) and `particle_spread_factor` as *cache values*, not authored input
- `dump_recipe_json()` emits `grind` and **omits** the derived scalars — `recipe_hash()` is taken over the authored distribution, not a derivation whose rounding could shift; when `grind` is absent, the key is omitted entirely so pre-existing recipe hashes don't change
- Sweeping `puck.particle_diameter_um` on a distribution-bearing recipe scales every bin by one factor (shape-preserving, lands d32 exactly on target); sweeping `puck.particle_spread_factor` on such a recipe is **refused** — there is no shape-preserving way to retarget the spread of a fixed distribution
- Bin diameters span 10-2000 µm (wider than the scalar envelope, since real coffee fines sit at 10-100 µm); the derived d32 must still land in 150-800 µm
- Bins are rounded to nanometre resolution so `load → dump → load → dump` is an exact fixed point

## CFD Solvers (Level 4, both outside the default pipeline)

- `espressolab_cli cfd --recipe <file> [--radial <n>] [--axial <n>] [--field pressure|saturation|temperature|tds]` — axisymmetric solver, `engine/cfd/`
- `espressolab_cli cfd3d --recipe <file> [--nx <n>] [--ny <n>] [--nz <n>] [--out <dir>]` — Cartesian 3D solver, `engine/cfd3d/`; mesh bounded to 128x128x256 and 262,144 cells total, x-fastest storage order
- CFD3D has explicit REST routes and independent artifacts/schemas: `case.json`, `summary.json`, `manifest.json`, `samples.csv`, `mesh.json`, `fields.elf3d`, `index.json`; the `ELF3D-1` snapshot format is little-endian float64, versioned independently from the standard shot result
- Neither solver's radial/3D structure or dynamic-channelling behavior feeds the default pipeline — `engine/cfd/` adds a radial coordinate but never touches the dashboard or default artifacts

## Testing

- Tags: `[grind]` `[grind_sim]` for the comminution model; `[cfd]` `[cfd3d]` for the solvers; `[axial]` `[regions]` for related puck-region logic — see the `build-and-test` skill for running a single tag

## Related Skills

- `build-and-test` (relevant tags)
- `data-contract-change` — grind specs/results and CFD3D cases/results are still data contracts requiring the full 6-step procedure when a field is added or changed, even though they carry no shot hash
