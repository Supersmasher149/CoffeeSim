---
name: data-contract-change
description: Walks the 6-step Contract Change Procedure for adding or changing any recipe, coefficient, result, sweep, measured-shot, CFD3D, or reference field: C++ domain type, loader/serializer/hash, schema plus REST docs, TypeScript plus UI, tests, and docs/data-contracts.md. Use whenever a field is added, renamed, removed, or reinterpreted in any JSON or CSV document EspressoLab reads or writes.
---

# Data Contract Change

## The 6 Steps

Source of truth: `docs/development.md` "Changing a Data Contract" and `docs/data-contracts.md` "Contract Change Procedure" — open the relevant doc section and follow it as authoritative; this list is the actionable summary.

1. **C++ domain type** — define the field, its units, ownership, default behavior, and version impact in the owning domain type: `Recipe`/`ModelCoefficients` in `types.hpp`, `ShotResult` in `result.hpp`, `Cfd3dCase`/`Cfd3dResult` in `cfd3d_artifact_io.hpp`, `SweepSpec`/`SweepResult` in `experiment.hpp`, `calibration::MeasuredShot`/`MeasuredSample`/`LossBreakdown` in `calibration.hpp`
2. **Loader/serializer/hash/manifest/validation** — update the relevant loader, serializer, hash-or-manifest behavior, and runtime validation (`artifact_io`, `cfd3d_artifact_io`, `artifact_io_sweep`, `calibration::io`, `engine/grind/grinder_io.cpp`)
3. **JSON schema + REST docs** — update `schemas/*.json` and `docs/api.md` in the same change; note schemas document intended format but aren't auto-enforced at runtime — the loader/serializer is the actual executable contract
4. **TypeScript types + UI** — update `web/src` types and every UI path in `web/src/features/` that consumes or edits the field
5. **Tests** — add loader/serializer round-trip tests, plus an API or UI test when the field crosses a process boundary (see the `build-and-test` skill)
6. **Docs** — update `docs/data-contracts.md` when ownership, units, or compatibility semantics change

## Which Layer Owns What

From CLAUDE.md's repository map / target table:

- Domain type + validation: `engine/espresso_core/` (`types.hpp`, `result.hpp`), `engine/cfd3d/`, `engine/experiment_runner/` (`experiment.hpp`), `engine/calibration/` (`calibration.hpp`), `engine/grind/`
- Serialize/hash: `engine/artifact_io/` (JSON/CSV, canonical hashes, manifests)
- REST translation: `apps/espressolab_server/`
- CLI file output: `apps/espressolab_cli/` (`workflows.cpp`)
- UI: `web/src/features/`
- Keep each concern in the lowest layer that can own it — never put a physics decision in `artifact_io`, or serialization logic in the model library

## Units Table

Apply when adding a physical field; external → internal:

- Dose and beverage mass: g → kg
- Basket diameter and puck depth: mm → m
- Particle diameter: µm → m
- Pressure: bar → Pa
- Temperature: °C → K
- Flow: ml/s → m³/s
- TDS and extraction yield: percent → fraction
- Convert only at the recipe/result boundary; never add browser-side formulae for authoritative metrics

## Hash and Determinism Impact

- Changing serialization order or the set of hashed fields changes reproducibility identity — this must be a deliberate, tested contract change; verify with the `acceptance-demo` skill before and after
- Conditionally omitting an optional field from serialization to avoid changing the hash of pre-existing documents is an established pattern here (see `puck.grind` in the `grind-cfd-workflow` skill) — consider it for new optional fields

## Special Cases

- Where schema text and runtime behavior differ, the loader/serializer is correct — fix the mismatch rather than treating the schema as authoritative
- `tests/schemas/schema_contract_check.py` cross-checks representative documents against `schemas/*.json` and the built CLI's loader/`validate()` path; needs the `jsonschema` pip package (not vendored), is not part of `./scripts/test.sh`, and must be run by hand
- Grind specs/results and CFD3D cases/results still require this procedure even though they carry no recipe/coefficient/result hash — see the `grind-cfd-workflow` skill for their specifics

## Related Skills

- `build-and-test` (step 5, run the relevant tags)
- `acceptance-demo` (verify hashes before/after)
- `grind-cfd-workflow` and `calibration-workflow` link back here whenever their work touches a contract field
- `pr-checklist` confirms all 6 steps were completed before opening a PR
