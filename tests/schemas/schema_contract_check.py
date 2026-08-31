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
import referencing
import referencing.jsonschema


# The shipped schemas cross-reference each other by $id (recipe.schema.json's
# `bean` is a $ref to bean-1.0.json), so a validator has to be handed the whole
# set. Validating against a bare schema silently fails to resolve those refs.
def schema_registry(schema_dir: Path):
    registry = referencing.Registry()
    for path in sorted(schema_dir.glob("*.json")):
        document = load_schema(path)
        if "$id" not in document:
            continue
        resource = referencing.Resource.from_contents(
            document, default_specification=referencing.jsonschema.DRAFT202012
        )
        registry = resource @ registry
    return registry


def load_schema(path: Path):
    with open(path) as f:
        return json.load(f)


def schema_accepts(schema, instance, registry=None) -> tuple[bool, str]:
    try:
        validator = jsonschema.Draft202012Validator(
            schema, registry=registry if registry is not None else referencing.Registry()
        )
        validator.validate(instance)
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

    registry = schema_registry(repo_root / "schemas")
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
        schema_ok, schema_msg = schema_accepts(recipe_schema, recipe, registry)
        if coefficients is not None:
            coeff_ok, coeff_msg = schema_accepts(coeff_schema, coefficients, registry)
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

    # 9-13. Grind: the scalar pair and the distribution are mutually exclusive
    #       spellings of one input, and the schema's oneOf must agree with
    #       load_recipe_json()'s CONFLICTING_FIELD rejection.
    psd_recipe = json.loads(
        (repo_root / "assets" / "recipes" / "psd-bimodal.json").read_text()
    )
    check(
        "valid_recipe_with_grind_distribution",
        psd_recipe,
        default_coeff,
        expect_schema=True,
        expect_cli=True,
        note="distribution replaces the scalar pair; loader derives d32 and the spread",
    )

    r = copy.deepcopy(psd_recipe)
    r["puck"]["particle_diameter_um"] = 350.0
    r["puck"]["particle_spread_factor"] = 0.55
    check(
        "invalid_recipe_both_grind_spellings",
        r,
        default_coeff,
        expect_schema=False,
        expect_cli=False,
        note="schema oneOf and the loader's CONFLICTING_FIELD agree",
    )

    r = copy.deepcopy(psd_recipe)
    del r["puck"]["grind"]
    check(
        "invalid_recipe_no_grind_spelling_at_all",
        r,
        default_coeff,
        expect_schema=False,
        expect_cli=False,
    )

    # A distribution every one of whose bins is individually legal, but whose
    # derived d32 falls outside the band the correlations were shaped around.
    r = copy.deepcopy(psd_recipe)
    r["puck"]["grind"]["bins"] = [
        {"diameter_um": 10.0, "mass_fraction": 0.5},
        {"diameter_um": 20.0, "mass_fraction": 0.5},
    ]
    check(
        "documented_divergence_grind_d32_out_of_band",
        r,
        default_coeff,
        expect_schema=True,
        expect_cli=False,
        note="schema cannot express the derived-d32 envelope; Recipe::validate() enforces it",
    )

    # Mass fractions summing to something other than 1: another cross-item rule
    # a JSON Schema array constraint cannot express.
    r = copy.deepcopy(psd_recipe)
    r["puck"]["grind"]["bins"] = [
        {"diameter_um": 250.0, "mass_fraction": 0.3},
        {"diameter_um": 450.0, "mass_fraction": 0.3},
    ]
    check(
        "documented_divergence_grind_mass_fraction_sum",
        r,
        default_coeff,
        expect_schema=True,
        expect_cli=False,
        note="schema cannot express the cross-item sum-to-1 rule; GrindDistribution::validate() enforces it",
    )

    # --- the flavour overlay's bean profile ---
    hologram = json.loads(
        (repo_root / "assets" / "beans" / "counter-culture-hologram.json").read_text()
    )
    bean_recipe = copy.deepcopy(baseline_recipe)
    bean_recipe["bean"] = hologram
    check(
        "valid_recipe_with_bean_profile",
        bean_recipe,
        default_coeff,
        expect_schema=True,
        expect_cli=True,
        note="optional inline bean; drives no physical quantity",
    )

    r = copy.deepcopy(bean_recipe)
    r["bean"]["classes"]["acids"]["mass_fraction"] = 2.0
    check(
        "invalid_bean_mass_fraction_out_of_range",
        r,
        default_coeff,
        expect_schema=False,
        expect_cli=False,
    )

    r = copy.deepcopy(bean_recipe)
    del r["bean"]["classes"]["lipids"]
    check(
        "invalid_bean_missing_solute_class",
        r,
        default_coeff,
        expect_schema=False,
        expect_cli=False,
        note="the six classes are a closed vocabulary; a missing one is an error, not a zero",
    )

    r = copy.deepcopy(bean_recipe)
    r["bean"]["classes"]["esters"] = {"mass_fraction": 0.0}
    check(
        "invalid_bean_unknown_solute_class",
        r,
        default_coeff,
        expect_schema=False,
        expect_cli=False,
        note="schema additionalProperties and the loader's UNKNOWN_SOLUTE_CLASS agree",
    )

    r = copy.deepcopy(bean_recipe)
    r["bean"]["classes"]["acids"]["mass_fraction"] = 0.20
    check(
        "documented_divergence_bean_mass_fraction_sum",
        r,
        default_coeff,
        expect_schema=True,
        expect_cli=False,
        note="schema cannot express the cross-item sum-to-1 rule; BeanProfile::validate() enforces it",
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
