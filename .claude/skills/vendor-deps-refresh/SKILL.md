---
name: vendor-deps-refresh
description: Wraps scripts/vendor_deps.sh to re-pin the vendored offline C++ dependencies (nlohmann/json, Catch2, cpp-httplib, FTXUI) into third_party/. Use when asked to bump or refresh a vendored dependency version or apply a security fix to one — this is low-frequency and higher-risk, so a clean rebuild and full test pass is mandatory afterward, not optional.
---

# Vendor Deps Refresh

## What It Does

`./scripts/vendor_deps.sh` downloads and overwrites (needs network access, unlike the offline build itself):

- `third_party/nlohmann/json.hpp` — nlohmann/json, currently pinned `v3.11.3`
- `third_party/catch2/catch_amalgamated.{hpp,cpp}` — Catch2, `v3.5.4`
- `third_party/httplib/httplib.h` — cpp-httplib, `v0.15.3`
- `third_party/ftxui/*` — FTXUI amalgamated release zip, `v7.0.3`
- Versions are hardcoded as `*_VERSION` variables at the top of the script itself

## Why It's Risky

- The entire point of vendoring is that a clean clone builds without fetching anything — CI's native job depends on this explicitly
- A version bump can introduce breaking API changes, especially FTXUI (affects the TUI directly) or Catch2 (affects test macros across the whole suite)

## Procedure

1. Edit the relevant `*_VERSION` variable in `scripts/vendor_deps.sh` for the dependency being bumped
2. Run `./scripts/vendor_deps.sh`
3. `git diff third_party/` to review exactly what changed
4. Rebuild clean: `rm -rf build && ./scripts/build.sh`
5. Run the full suite: `./scripts/test.sh`
6. If FTXUI was bumped, also run `python3 tests/pty/tui_smoke.py`
7. Update `third_party/README.md` if it records pinned versions
8. Commit `third_party/` changes together with the version bump

## Related Skills

- `build-and-test` — the full rebuild + full test suite after a refresh is a hard requirement of this skill, not a suggestion
