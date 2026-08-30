---
name: dev-server
description: Runs scripts/dev.sh — the REST tool server on port 8734 alongside the Vite dashboard dev server on port 5173 — for manually exercising dashboard changes. Use when asked to run or preview the dashboard, or when a UI change (dragging, selection, chart sync, downloads, accessibility) needs manual verification because no automated browser-interaction test harness exists in this repo.
---

# Dev Server

## What It Runs

`./scripts/dev.sh`:

- Builds native Release if `espressolab_server` isn't already built
- Runs `npm install` in `web/` if `node_modules` is missing
- Starts `build/apps/espressolab_server/espressolab_server --assets assets --references espresso_real_world_refs --port 8734` in the background (killed via `trap` on script exit)
- Runs `cd web && npm run dev` (Vite, serving `http://localhost:5173`)

## When You Need It

- No automated browser-interaction test harness exists — `npm run typecheck` and `npm run build` only check types and that the app compiles, not runtime behavior
- Per `docs/development.md`'s Test Selection section: manually verify dragging, selection, chart synchronization, downloads, and accessibility whenever they change
- After any `web/src` change affecting user interaction, not just static rendering

## Manual Verification Notes

- The dashboard at `:5173` calls the REST API at `:8734`; both must be running (which `dev.sh` does for you)
- Stop with Ctrl-C — the script's own `trap` kills the server process on exit

## Related Skills

- `build-and-test` — typecheck/build are compile-time only and don't replace this manual pass
- `pr-checklist` — cite a `dev-server` session as your "manually exercised" evidence for dashboard changes
