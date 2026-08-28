#!/usr/bin/env python3
"""Audit P6, issue #19: verify schemas/*.json agrees with the C++ loader/
validate() contract (engine/artifact_io/json_io.cpp,
engine/espresso_core/types.cpp) on representative valid and invalid
documents, and confirm the specific divergences the schema now documents
(ordering, cross-field sums) are exactly as documented -- not silent bugs.

Not part of ./scripts/test.sh: it depends on the third-party `jsonschema`
PyPI package, which is not vendored for the project's offline build (see
CLAUDE.md "Requirements"). Run by hand from a venv, after `./scripts/build.sh`:

    python3 -m venv /tmp/venv19 && /tmp/venv19/bin/pip install jsonschema
    /tmp/venv19/bin/python tests/schemas/schema_contract_check.py [path/to/espressolab_cli]
"""

import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import jsonschema


def load_schema(path: Path):
    with open(path) as f:
        return json.load(f)


def schema_accepts(schema, instance) -> tuple[bool, str]:
    try:
        jsonschema.validate(instance=instance, schema=schema)
        return True, ""
    except jsonschema.ValidationError as e:
        return False, e.message


def cli_accepts(cli: str, recipe: dict, coefficients: dict | None) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory() as tmp:
        recipe_path = Path(tmp) / "recipe.json"
        recipe_path.write_text(json.dumps(recipe))
        args = [cli, "simulate", "--recipe", str(recipe_path)]
        if coefficients is not None:
            coeff_path = Path(tmp) / "coefficients.json"
            coeff_path.write_text(json.dumps(coefficients))
            args += ["--coefficients", str(coeff_path)]
        result = subprocess.run(args, capture_output=True, text=True, timeout=30)
        return result.returncode == 0, (result.stderr or result.stdout).strip()[:200]


def default_binary(repo_root: Path) -> str:
    return str(repo_root / "build" / "apps" / "espressolab_cli" / "espressolab_cli")


def main():
    repo_root = Path(__file__).resolve().parents[2]
    cli = sys.argv[1] if len(sys.argv) > 1 else default_binary(repo_root)

    recipe_schema = load_schema(repo_root / "schemas" / "recipe.schema.json")
    coeff_schema = load_schema(repo_root / "schemas" / "coefficients.schema.json")
    baseline_recipe = json.loads(
        (repo_root / "assets" / "recipes" / "baseline.json").read_text()
    )
    default_coeff = json.loads(
        (repo_root / "assets" / "coefficients" / "default-v1.json").read_text()
    )

    failures = []
    checked = 0

    def check(name, recipe, coefficients, expect_schema, expect_cli, note=""):
        nonlocal checked
        checked += 1
        schema_ok, schema_msg = schema_accepts(recipe_schema, recipe)
        if coefficients is not None:
            coeff_ok, coeff_msg = schema_accepts(coeff_schema, coefficients)
            schema_ok = schema_ok and coeff_ok
            schema_msg = schema_msg or coeff_msg
        cli_ok, cli_msg = cli_accepts(cli, recipe, coefficients)
        status = (
            "OK"
            if (schema_ok == expect_schema and cli_ok == expect_cli)
            else "MISMATCH"
        )
        if status == "MISMATCH":
            failures.append(
                f"{name}: expected schema={expect_schema} cli={expect_cli}, "
                f"got schema={schema_ok} ({schema_msg}) cli={cli_ok} ({cli_msg})"
            )
        print(
            f"[{status}] {name}: schema={schema_ok} cli={cli_ok}"
            + (f"  -- {note}" if note else "")
        )

    # 1. Straightforwardly valid: both must accept.
    check(
        "valid_baseline_recipe_and_coefficients",
        baseline_recipe,
        default_coeff,
        expect_schema=True,
        expect_cli=True,
    )

    # 2. schema_version omitted entirely: the fix under test for the recipe
    #    side of #19 -- load_recipe_json() defaults it, schema must too.
    r = copy.deepcopy(baseline_recipe)
    del r["schema_version"]
    check(
        "valid_recipe_no_schema_version",
        r,
        default_coeff,
        expect_schema=True,
        expect_cli=True,
        note="loader defaults absent schema_version; schema no longer requires it",
    )

    # 3. Missing puck: both must reject.
    r = copy.deepcopy(baseline_recipe)
    del r["puck"]
    check(
        "invalid_recipe_missing_puck",
        r,
        default_coeff,
        expect_schema=False,
        expect_cli=False,
    )

    # 4. dose_g out of range: both must reject.
    r = copy.deepcopy(baseline_recipe)
    r["puck"]["dose_g"] = 5.0
    check(
        "invalid_recipe_dose_out_of_range",
        r,
        default_coeff,
        expect_schema=False,
        expect_cli=False,
    )

    # 5. One coefficient value field omitted: the fix under test for the
    #    coefficients side of #19 -- previously schema-valid (every `values`
    #    property was individually optional) but loader-rejected.
    c = copy.deepcopy(default_coeff)
    del c["values"]["grind_exponent"]
    check(
        "invalid_coefficients_missing_one_value_field",
        baseline_recipe,
        c,
        expect_schema=False,
        expect_cli=False,
        note="pre-fix this was schema=True cli=False -- the mismatch #19 reports",
    )

    # 6. Coefficient value out of range: both must reject.
    c = copy.deepcopy(default_coeff)
    c["values"]["initial_porosity"] = 2.0
    check(
        "invalid_coefficients_out_of_range",
        baseline_recipe,
        c,
        expect_schema=False,
        expect_cli=False,
    )

    # 7. KNOWN, DOCUMENTED DIVERGENCE: non-increasing profile times. Each
    #    point is individually in range, so the schema (which cannot express
    #    item-to-item ordering) accepts it; PiecewiseLinearProfile::validate()
    #    rejects it. This proves the $comment in recipe.schema.json is
    #    accurate, not a leftover gap.
    r = copy.deepcopy(baseline_recipe)
    r["pressure_profile_bar"] = [[0, 2], [5, 4], [3, 6]]
    check(
        "documented_divergence_unordered_profile_times",
        r,
        default_coeff,
        expect_schema=True,
        expect_cli=False,
        note="schema cannot express strictly-increasing time; documented in $defs.profile_pressure",
    )

    # 8. KNOWN, DOCUMENTED DIVERGENCE: parallel region area_fractions that
    #    don't sum to 1. Each region is individually in range, so the schema
    #    accepts it; Recipe::validate() rejects it.
    r = copy.deepcopy(baseline_recipe)
    r["parallel_regions"] = [{"area_fraction": 0.5, "permeability_multiplier": 1.0}]
    check(
        "documented_divergence_region_area_sum",
        r,
        default_coeff,
        expect_schema=True,
        expect_cli=False,
        note="schema cannot express the cross-item sum-to-1 rule; documented in the property description",
    )

    print(f"\n{checked - len(failures)}/{checked} cases matched expectations.")
    if failures:
        print("\nFAILURES:")
        for f in failures:
            print(f"  - {f}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
