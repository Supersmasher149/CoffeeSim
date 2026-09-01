---
name: perf-branch-workflow
description: Investigates or fixes a performance regression/hotspot in an isolated git worktree, off origin/main, without touching the current checkout's uncommitted state — profiles before and after, and gates merge readiness on the CI "Performance budget" step (espressolab_cli bench) and the acceptance-demo determinism/hash check. Use when asked to investigate a slow path, optimize a hotspot, or verify a change doesn't regress simulation or dashboard performance.
---

# Performance Branch Workflow

Performance work happens on its own branch, in its own worktree, profiled
before and after, and is never considered mergeable on "it built" alone —
it must be measured against this repo's actual performance budget.

## Isolate the work

Never make performance changes in a dirty current checkout. Create a
separate worktree on its own branch, off `origin/main`:

```bash
git worktree add -b perf/<slug> <path-outside-the-repo> origin/main
```

If the current checkout has uncommitted changes, leave them exactly as they
are — do not stash, revert, or fold them into the perf branch. This mirrors
the same worktree-isolation discipline the `repo-orchestrator` skill uses
for implementation tasks.

## Baseline before changing anything

Build and profile `origin/main` first, in its own worktree if you need a
clean baseline separate from the perf branch (e.g.
`git worktree add --detach <baseline-path> origin/main`). Record:

- Wall-clock/allocation traces under `profiling/` (e.g. Instruments `.trace`
  bundles, or equivalent for the platform you're on).
- Structured before/after numbers under `results/` alongside whatever
  artifact the change touches (a `cfd3d` case directory, a bundle-size
  report, etc.) — follow the existing naming pattern already there
  (`results/cfd3d-baseline/`, `results/cfd3d-heavy/`, etc.) rather than
  inventing a new layout.

Skipping the baseline capture is the single most common way this kind of
work produces an unverifiable "it feels faster" claim — don't skip it.

## Make the change, then re-measure

Implement the change on the perf branch. Re-run the same profiling and
benchmark steps used for the baseline, using identical inputs, and compare
the numbers explicitly in whatever you report — not "it built and passed
tests," but "before: X, after: Y."

## Gate merge readiness

Before calling a perf branch done, all of the following must hold:

1. `./scripts/build.sh` and the relevant `[tag]` test(s) pass (see
   `build-and-test`).
2. The CI "Performance budget" step passes: `espressolab_cli bench --repeats
   50` (`.github/workflows/ci.yml`), or run it locally with
   `./build/apps/espressolab_cli/espressolab_cli bench --repeats 50` and
   compare against the documented budget (currently a 60 s, 100 Hz shot at
   ~0.468 ms median, well inside a 20 ms budget — see
   `docs/current-state-and-gaps.md` and `docs/roadmap.md`). A perf change
   that pushes this number *up* without an explicit reason is a regression,
   not a win.
3. `acceptance-demo`'s determinism check passes: a performance change must
   never alter the result hash on the same build. If it does, it changed
   physics or serialization, not just speed, and belongs through the
   `data-contract-change` procedure instead — stop and re-scope.
4. Web-side performance changes (bundle splitting, lazy loading, etc.) are
   verified with `npm --prefix web run build` and the actual emitted
   chunk/gzip sizes compared before/after, not assumed from the diff.

## Clean up

Once merged or abandoned, remove the worktree(s):

```bash
git worktree remove <path>
```

Leave `profiling/` traces and `results/` captures that back up the recorded
before/after numbers; prune ones that were only exploratory scratch.

## Related Skills

- `build-and-test` — the build and `[tag]` test pass in step 1.
- `acceptance-demo` — the determinism/hash check in step 3.
- `repo-orchestrator` — if this perf work is one iteration of the broader
  scout-to-PR loop, use that skill's task-packet and verification structure
  around this procedure rather than running it standalone.
