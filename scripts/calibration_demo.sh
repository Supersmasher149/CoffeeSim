#!/usr/bin/env bash
# Exercises the whole calibration path from the command line: generate synthetic
# shots from known coefficients, start the fit from a deliberately wrong point,
# and check that it recovers the truth.
#
# This validates the machinery. It says nothing about real espresso: the "shots"
# are the model's own output.
set -euo pipefail
cd "$(dirname "$0")/.."

CLI=build/apps/espressolab_cli/espressolab_cli
[ -x "$CLI" ] || ./scripts/build.sh Release

WORK="${TMPDIR:-/tmp}/espressolab-calibration-demo"
rm -rf "$WORK"
mkdir -p "$WORK/shots"

TRUTH=assets/coefficients/default-v1.json

echo "== generating synthetic shots from ${TRUTH}"
for recipe in baseline pre-infusion immediate-pressure; do
  "$CLI" synthesize \
    --recipe "assets/recipes/${recipe}.json" \
    --coefficients "$TRUTH" \
    --noise 0.05 --seed 7 \
    --recipe-path "$(pwd)/assets/recipes/${recipe}.json" \
    --out "$WORK/shots/${recipe}.json" > /dev/null
done
echo "   3 shots in $WORK/shots"

echo
echo "== perturbing the starting point away from the truth"
python3 - "$TRUTH" "$WORK/start.json" <<'PY'
import json, sys
truth = json.load(open(sys.argv[1]))
truth["id"] = "perturbed-start"
truth["values"]["kozeny_constant"] *= 3.0
truth["values"]["extraction_rate_ref_s"] *= 0.4
json.dump(truth, open(sys.argv[2], "w"), indent=2)
PY
python3 -c "
import json,sys
t=json.load(open('$TRUTH'))['values']; s=json.load(open('$WORK/start.json'))['values']
for k in ('kozeny_constant','extraction_rate_ref_s'):
    print(f'   {k}: start {s[k]:.6g}  truth {t[k]:.6g}')"

echo
echo "== fitting (holding out immediate-pressure)"
"$CLI" calibrate \
  --shots "$WORK/shots" \
  --coefficients "$WORK/start.json" \
  --fit kozeny_constant,extraction_rate_ref_s \
  --holdout immediate-pressure \
  --out "$WORK/fitted.json" \
  --report "$WORK/report.json"

echo
python3 - "$TRUTH" "$WORK/fitted.json" <<'PY'
import json, sys
truth = json.load(open(sys.argv[1]))["values"]
fitted = json.load(open(sys.argv[2]))["values"]

ok = True
for name in ("kozeny_constant", "extraction_rate_ref_s"):
    error = abs(fitted[name] - truth[name]) / truth[name]
    status = "PASS" if error < 0.02 else "FAIL"
    if error >= 0.02:
        ok = False
    print(f"{status}  {name}: recovered {fitted[name]:.6g} vs truth {truth[name]:.6g} "
          f"({error * 100:.2f}% error)")

sys.exit(0 if ok else 1)
PY
