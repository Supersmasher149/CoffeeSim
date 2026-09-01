# Real-shot validation, phase 2: what's next

## This session, briefly

Implemented `docs/superpowers/specs/2026-09-01-real-shot-telemetry-design.md`
in full:

- **Acquisition spike resolved immediately on path 2, not Visualizer.** The
  Coffee Ad Astra article behind the four `espresso_real_world_refs/*.json`
  records links a zipped Dropbox archive of the author's 11 raw DE1 `.shot`
  exports, and all four target files were in it. Verified by matching
  `drink_weight`/`scale_weight` and `profile_notes` against the already-
  committed shot-level metadata.
- **New `tools/import_shot_telemetry.py`** (stdlib-only) parses the DE1
  `.shot` format and populated `timeseries` + `observed.final_shot_time_s`
  for all four Gagné EG-1/Niche shots. Raw `.shot` files are not vendored
  (no explicit license on the archive) — only derived numeric samples are.
- **`telemetry_available` was hardcoded `false` everywhere** (C++ and TS) —
  now computed per-reference and catalogue-wide from actual content.
- **Schema refinements the real data forced**: `tds_raw_percent` /
  `tds_filtered_percent` / `tds_uncertainty_percent_points` added to the
  capture schema (`tds_method.filtered` names which is authoritative,
  checked for consistency); the "every TDS reading must be kept" rule
  scoped to `status == "measured"` (an `external_public` record only has a
  published aggregate to transcribe).
- Verified: full Catch2 suite (229 cases / 88,154 assertions),
  `real_shot_capture_check.py`, `scripts/demo.sh` (hash unchanged), web
  typecheck/`test:coverage`/build. Importer re-run is byte-identical.
- Committed as `68a2747` on `feat/real-shot-capture-template`, pushed to
  origin.

**No calibration, no validation claim** — unchanged. These four shots still
lack basket diameter, puck depth, PSD, and a measured brew temperature.

## Load-bearing fact for next session

`feat/real-shot-capture-template`'s first commit (`1089957`, the PR #56
capture-template work) is **already merged into `main`** (merge commit
`899d3a7`). This session's commit (`68a2747`) sits on top of it and is
**not yet in `main`** — it needs its own new PR, not a reopen of #56. `main`
is otherwise caught up to this branch, so it's a clean one-commit PR.

## Immediate next step

Open a PR from `feat/real-shot-capture-template` (HEAD `68a2747`) into
`main` for this session's telemetry work.

## Deferred, not done

- **Step 2B** (Visualizer-JSON → `external_public` capture record) was
  never exercised — 2A resolved first. `tools/import_shot_telemetry.py`'s
  docstring says so explicitly. Only add a Visualizer-JSON parser branch
  against an actual Visualizer export, not speculatively.
- `assets/real_shots/` still holds only the `pending_capture` fixture — no
  `measured` or `external_public` capture record exists yet.
- The ~2000 lines of unrelated pre-existing uncommitted work in this
  worktree (README.md, `engine/cfd3d/cfd3d_solver.cpp`, the web dashboard
  revamp files, etc.) were deliberately left untouched all session. Do not
  fold them into the PR above; they're a separate thread.

## Candidate follow-on work (pick one; don't assume which)

1. **First-party capture**: brew a real shot per the protocol in
   `docs/real-shot-validation.md`, promote it to
   `assets/real_shots/*.capture.json` with `status: "measured"`.
2. **Reconcile specs**: `docs/superpowers/specs/2026-08-25-real-shot-validation-design.md`
   may overlap or contradict the telemetry work now implemented — check on
   first touch.
3. **Visualizer parser**, only if a future shot's telemetry is unreachable
   any other way (build it against a real sample, per the note above).
