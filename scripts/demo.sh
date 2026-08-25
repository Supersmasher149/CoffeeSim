#!/usr/bin/env bash
# The acceptance run from section 16.2: baseline shot, grind sweep, exported
# artifacts, and a determinism check that the same inputs reproduce the same
# result hash.
set -euo pipefail
cd "$(dirname "$0")/.."

CLI=build/apps/espressolab_cli/espressolab_cli
[ -x "$CLI" ] || ./scripts/build.sh Release

echo "== baseline shot"
"$CLI" simulate \
  --recipe assets/recipes/baseline.json \
  --coefficients assets/coefficients/default-v1.json \
  --out outputs/shots/baseline

echo
echo "== grind-size sweep"
"$CLI" sweep --spec assets/sweeps/grind-size.json --out outputs/sweeps/grind-size

echo
echo "== determinism check"
"$CLI" simulate \
  --recipe assets/recipes/baseline.json \
  --coefficients assets/coefficients/default-v1.json \
  --out outputs/shots/baseline-rerun

FIRST=$(python3 -c "import json;print(json.load(open('outputs/shots/baseline/manifest.json'))['result_hash'])")
SECOND=$(python3 -c "import json;print(json.load(open('outputs/shots/baseline-rerun/manifest.json'))['result_hash'])")

if [ "$FIRST" = "$SECOND" ]; then
  echo "PASS  identical result hash: $FIRST"
else
  echo "FAIL  hashes differ: $FIRST vs $SECOND"
  exit 1
fi
