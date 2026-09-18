#!/usr/bin/env bash
# Same-build bit-identity check for solver refactors. Simulates every recipe in
# assets/recipes/ with no bean and with each bean in assets/beans/, and prints
# one line per case: the result_hash plus a digest of every artifact file
# (minus the wall-clock timestamp), so the flavour series and the samples are
# covered even where result_hash does not reach.
#
# Tests guard invariants, not bits, so a refactor can change the last ulp -- for
# example by splitting an a * b + c the compiler would fuse into an FMA -- and
# still pass the whole suite. Record before, record after, compare:
#
#   ./scripts/solver_hash_matrix.sh > before.txt      # on the base commit
#   ./scripts/solver_hash_matrix.sh --against before.txt
#
# Hashes are per-toolchain (docs/data-contracts.md): compare only runs from the
# same machine and compiler, never against a committed file.
set -euo pipefail
cd "$(dirname "$0")/.."

CLI=build/apps/espressolab_cli/espressolab_cli
[ -x "$CLI" ] || ./scripts/build.sh Release >&2

AGAINST=""
if [ "${1:-}" = "--against" ]; then
  AGAINST="${2:?usage: $0 [--against <recorded.txt>]}"
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

record() {
  local recipe bean name out hash digest
  for recipe in assets/recipes/*.json; do
    for bean in none assets/beans/*.json; do
      name="$(basename "$recipe" .json)+$(basename "$bean" .json)"
      out="$WORK/$name"
      if [ "$bean" = none ]; then
        "$CLI" simulate --recipe "$recipe" --out "$out" >/dev/null 2>&1 || true
      else
        "$CLI" simulate --recipe "$recipe" --bean "$bean" --out "$out" >/dev/null 2>&1 || true
      fi
      if [ -f "$out/manifest.json" ]; then
        hash=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['result_hash'])" \
          "$out/manifest.json")
        digest=$(cat "$out"/* | grep -v '"timestamp_utc"' | shasum -a 256 | cut -c1-16)
      else
        hash=FAILED digest=-
      fi
      echo "$name $hash $digest"
    done
  done
}

if [ -z "$AGAINST" ]; then
  record
  exit 0
fi

record > "$WORK/current.txt"
if diff "$AGAINST" "$WORK/current.txt"; then
  echo "PASS  $(wc -l < "$WORK/current.txt" | tr -d ' ') cases bit-identical" >&2
else
  echo "FAIL  results differ from $AGAINST (lines above: < recorded, > current)" >&2
  exit 1
fi
