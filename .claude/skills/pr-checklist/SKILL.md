---
name: pr-checklist
description: Walks a diff against EspressoLab's Pull-Request Checklist from CLAUDE.md and docs/development.md before opening or finishing a PR — dependency direction, deterministic hashes, finite input validation, regression tests, minimal relevant checks, and an explicit verification statement. Use when asked to prepare, review, or finalize a PR, or before stating a change is complete.
---

# PR Checklist

## The Checklist

Source of truth: `CLAUDE.md` "Pull-request checklist" / `docs/development.md` "Pull-Request Checklist".

1. **Dependency direction intact** — no UI or HTTP decisions in the solver
2. **Deterministic ordering, canonical serialization, and result hashes preserved** unless a versioned compatibility change is intended
3. **Finite numeric inputs validated** before they reach a numerical loop
4. **A regression test added for every fixed defect**, especially malformed input or an invariant violation
5. **Smallest relevant native, web, and documentation checks run** before review (not necessarily the full suite)
6. **A final explicit statement** of whether the change is verified by a test, only manually exercised, or not yet validated against real measurements

## Walking a Diff

- `git diff` (or diff against the target branch) to see everything changed
- For each changed file under `engine/*/` or `apps/*/`, identify its owning layer from CLAUDE.md's Target/Owns/Must-not-own table and confirm no upward dependency was introduced (e.g. no HTTP/React types leaking into `espresso_core` or `model_library`)
- If `artifact_io`, hashing, or serialization order was touched, run the `acceptance-demo` skill and record the result
- If a data-contract field (recipe/coefficients/result/sweep/measured-shot/CFD3D/reference) changed, confirm the full `data-contract-change` procedure was followed end to end
- If a defect was fixed, confirm a new test exists covering it and note which tag it lives under (see `build-and-test`)

## Verification Statement Template

- `"Verified by: [test tag(s) / demo script(s) run]"`, or
- `"Manually exercised via [dev-server / specific CLI command]"`, or
- `"Not yet validated against real measurements (synthetic/uncalibrated data only)"`

## Related Skills

- `build-and-test` (item 5, smallest relevant checks)
- `acceptance-demo` (item 2, hash preservation)
- `data-contract-change` (any contract field touched)
- `calibration-workflow` (if calibration code/data touched, cite synthetic vs. real-shot evidence explicitly)
