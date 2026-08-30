---
name: acceptance-demo
description: Runs scripts/demo.sh, the acceptance/determinism check that simulates a baseline shot, runs a grind-size sweep, reruns the baseline, and confirms both runs produce an identical result hash. Use before claiming a change doesn't affect reproducibility or determinism, and after touching the solver, artifact hashing, serialization order, or the CLI simulate/sweep paths.
---

# Acceptance Demo

## What It Does

`./scripts/demo.sh` (builds Release first if the CLI binary is missing) runs, in order:

1. **Baseline shot** — `espressolab_cli simulate --recipe assets/recipes/baseline.json --coefficients assets/coefficients/default-v1.json --out outputs/shots/baseline`
2. **Grind-size sweep** — `espressolab_cli sweep --spec assets/sweeps/grind-size.json --out outputs/sweeps/grind-size`
3. **Rerun** of the identical baseline shot into `outputs/shots/baseline-rerun`
4. **Determinism check** — compares `result_hash` from `outputs/shots/baseline/manifest.json` against `outputs/shots/baseline-rerun/manifest.json`

## Reading the Result

- `PASS  identical result hash: <hash>` — reproducibility preserved
- `FAIL  hashes differ: <hash1> vs <hash2>` (nonzero exit) — either an unintended nondeterminism bug, or a deliberate, versioned contract change that must be called out explicitly rather than silently landing
- This is CI's "Acceptance demo" step and the project's stated "definition of done" acceptance run

## When to Run

- Before stating a change doesn't affect reproducibility/determinism
- After touching `engine/espresso_core/`, `engine/artifact_io/`, hashing or serialization order, or sweep execution order
- As evidence for the `pr-checklist` bullet on preserving deterministic ordering/hashes

## Related Skills

- `pr-checklist` — the "preserve result hashes" item points here for verification
- `data-contract-change` — a genuine, intended hash change surfaced here is the signal that the full 6-step contract-change procedure applies
