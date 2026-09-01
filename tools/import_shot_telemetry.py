#!/usr/bin/env python3
"""Import DE1 shot-file telemetry into espresso_real_world_refs/*.json records.

Populates the `timeseries` array and `observed.final_shot_time_s` field of a
real-world reference record (`espresso_real_world_refs/*.json`, loaded by
`engine/reference_io/reference_io.cpp`) from a Decent DE1 `.shot` telemetry
export -- the same format `visualizer.coffee` and the DE1 app itself produce.
This is NOT the `assets/real_shots/*.capture.json` capture format (see
`schemas/real-shot-capture.schema.json`); the reference-shot format has no
JSON Schema of its own, `reference_io.cpp`'s `validate_document` is its
executable contract.

Deliberately stdlib only -- Python, not C++, because the repo must build
offline from a clean clone and an HTTPS client would mean a new vendored
dependency (see `tests/schemas/`, `tests/pty/`, `tests/cli/` for the same
precedent). This tool does no network access itself: point it at a `.shot`
file you already have locally.

DE1 `.shot` format: a flat sequence of top-level `name {v1 v2 ...}` float
arrays (one per physical line, unindented), followed by nested `settings {`
and `machine {` blocks (tab-indented key/value pairs, some of them further
nested). Only the five unindented top-level arrays this project's telemetry
contract needs are read; everything in `settings`/`machine` (accelerometer
calibration, Bluetooth address, per-machine app preferences, ...) is ignored.

Where the source files came from for the four shots this repo currently
carries: the article linked at each record's `source.article_url` links a
zipped package of the author's 11 raw DE1 `.shot` files
(https://www.dropbox.com/s/l0v4a5d3h98kasn/shotfiles_Mar2_2021.zip). The
article states no explicit license for that archive; this tool reproduces
only derived numeric samples from it (not the raw export files themselves),
under the same public-sharing precedent already used for the shot-level
metadata (dose, TDS, yield, ...) already committed in this catalogue -- see
`docs/real-shot-validation.md`.

Visualizer-JSON input (`visualizer.coffee`'s own export/API shape) is
mentioned as a possible second input format in the design this tool
implements, but is NOT implemented here: the acquisition spike for the four
shots this repo needed resolved via the DE1 `.shot` files directly, so a
Visualizer parser was never exercised against a real sample and would be
untested guesswork. Add it only against an actual Visualizer export.

Usage:
    # One shot file into one reference record:
    python3 tools/import_shot_telemetry.py \\
        --shot-file /path/to/20210302T094131.shot \\
        --reference espresso_real_world_refs/gagne_eg1_01.json

    # Every (shot file, reference record) pair named by manifest.json's
    # source_shot_file field, reading shot files from a local directory
    # (not committed to the repo -- see docs/real-shot-validation.md for how
    # to obtain them):
    python3 tools/import_shot_telemetry.py --all --shot-dir /path/to/shotfiles
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
REFERENCE_DIR = REPO_ROOT / "espresso_real_world_refs"

# espresso_real_world_refs timeseries_fields column name -> DE1 .shot field name.
DE1_FIELD_MAP = {
    "time_s": "espresso_elapsed",
    "pressure_bar": "espresso_pressure",
    "flow_ml_s": "espresso_flow",
    "beverage_mass_g": "espresso_weight",
    # "mix" (water temperature at the group mixing valve, entering the puck)
    # is the DE1's standard brew-temperature telemetry -- what visualizer.coffee
    # and the DE1 app itself chart as "Temp". espresso_temperature_basket is a
    # separate, firmware-estimated puck-temperature model this tool does not use.
    "temperature_c": "espresso_temperature_mix",
}

ROUND_DIGITS = 3

# json.dumps(indent=2) puts every array element on its own line, which turns
# a 390-sample timeseries into ~2000 lines of one-number-per-line noise. This
# collapses any purely-numeric JSON array (never a string array, so
# timeseries_fields is untouched) back onto a single line: one telemetry row
# per line, matching how a human would hand-author such a table.
_NUMBER_ROW = re.compile(r"\[\n\s*(-?\d[\d.eE+-]*(?:,\n\s*-?\d[\d.eE+-]*)*)\n\s*\]")


def _collapse_number_row(match: re.Match) -> str:
    numbers = re.split(r",\s*\n\s*", match.group(1))
    return "[" + ", ".join(n.strip() for n in numbers) + "]"


def compact_number_rows(text: str) -> str:
    return _NUMBER_ROW.sub(_collapse_number_row, text)


class ShotParseError(ValueError):
    pass


def parse_de1_shot(text: str) -> dict[str, list[float]]:
    """Extract the top-level float arrays this project's telemetry columns
    need from a DE1 `.shot` export.

    Anchored at line start/end (`re.MULTILINE`) so a same-named key nested
    under `settings {` or `machine {` (tab-indented) is never matched --
    only the unindented top-level array is.
    """
    fields: dict[str, list[float]] = {}
    for column, de1_name in DE1_FIELD_MAP.items():
        match = re.search(
            rf"^{re.escape(de1_name)} \{{([^{{}}]*)\}}$", text, re.MULTILINE
        )
        if not match:
            raise ShotParseError(f"field {de1_name!r} not found in shot file")
        try:
            values = [float(v) for v in match.group(1).split()]
        except ValueError as e:
            raise ShotParseError(
                f"field {de1_name!r} contains a non-numeric sample: {e}"
            ) from e
        if not values:
            raise ShotParseError(f"field {de1_name!r} is empty")
        fields[column] = values

    lengths = {column: len(values) for column, values in fields.items()}
    if len(set(lengths.values())) != 1:
        raise ShotParseError(f"telemetry arrays have mismatched lengths: {lengths}")
    return fields


def build_timeseries(
    fields: dict[str, list[float]], columns: list[str]
) -> list[list[float]]:
    """Row-major table in `columns` order, matching the reference record's
    own `timeseries_fields` (its declared column order for `timeseries`)."""
    count = len(next(iter(fields.values())))
    return [
        [round(fields[column][i], ROUND_DIGITS) for column in columns]
        for i in range(count)
    ]


def import_into_reference(shot_path: Path, reference_path: Path) -> bool:
    """Populate `timeseries` and `observed.final_shot_time_s` in one
    reference record from one DE1 shot file. Returns True if the record's
    content changed (idempotent: re-running against the same input and the
    already-updated record is a no-op)."""
    fields = parse_de1_shot(shot_path.read_text())
    reference = json.loads(reference_path.read_text())

    columns = reference["timeseries_fields"]
    if set(columns) != set(DE1_FIELD_MAP):
        raise ShotParseError(
            f"{reference_path.name}: timeseries_fields {columns} do not match the columns "
            f"this importer knows how to parse ({list(DE1_FIELD_MAP)})"
        )

    rows = build_timeseries(fields, columns)
    final_shot_time_s = round(fields["time_s"][-1], ROUND_DIGITS)

    before = json.dumps(reference, sort_keys=True)

    reference["timeseries"] = rows
    reference["observed"]["final_shot_time_s"] = final_shot_time_s
    reference["source"]["data_quality"]["timeseries"] = (
        f"populated from the original DE1 shot file ({shot_path.name}), downloaded from the "
        "zipped shot-file package linked in source.article_url. No explicit license was stated "
        "for that archive; this record reproduces only these derived numeric samples, under the "
        "same public-sharing precedent as the rest of this record's shot-level metadata (see "
        "docs/real-shot-validation.md). Columns time_s/pressure_bar/flow_ml_s/beverage_mass_g/"
        "temperature_c read directly from the shot file's espresso_elapsed/espresso_pressure/"
        "espresso_flow/espresso_weight/espresso_temperature_mix arrays via "
        "tools/import_shot_telemetry.py."
    )
    reference["source"]["data_quality"]["final_shot_time_s"] = (
        "the final sample of the shot file's espresso_elapsed array (pump-on time origin)."
    )

    after = json.dumps(reference, sort_keys=True)
    reference_path.write_text(
        compact_number_rows(json.dumps(reference, indent=2)) + "\n"
    )
    return before != after


def default_pairs(shot_dir: Path) -> list[tuple[Path, Path]]:
    """(shot_file, reference_file) pairs named by manifest.json's
    source_shot_file field, resolved against `shot_dir`."""
    manifest = json.loads((REFERENCE_DIR / "manifest.json").read_text())
    pairs = []
    for entry in manifest["references"]:
        shot_path = shot_dir / entry["source_shot_file"]
        reference_path = REFERENCE_DIR / entry["file"]
        pairs.append((shot_path, reference_path))
    return pairs


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--shot-file", type=Path, help="Path to a single DE1 .shot file"
    )
    parser.add_argument(
        "--reference",
        type=Path,
        help="espresso_real_world_refs/*.json record to populate",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Import every pair named by manifest.json's source_shot_file field",
    )
    parser.add_argument(
        "--shot-dir",
        type=Path,
        help="Directory holding the .shot files named in manifest.json (used with --all)",
    )
    args = parser.parse_args()

    if args.all:
        if args.shot_file or args.reference:
            parser.error("--all cannot be combined with --shot-file/--reference")
        if not args.shot_dir:
            parser.error("--all requires --shot-dir")
        pairs = default_pairs(args.shot_dir)
    else:
        if not args.shot_file or not args.reference:
            parser.error(
                "either --all --shot-dir=..., or both --shot-file and --reference, are required"
            )
        pairs = [(args.shot_file, args.reference)]

    changed = 0
    for shot_path, reference_path in pairs:
        if not shot_path.is_file():
            print(
                f"FAIL {reference_path.name}: no shot file at {shot_path}",
                file=sys.stderr,
            )
            return 1
        if not reference_path.is_file():
            print(f"FAIL {reference_path}: no reference record there", file=sys.stderr)
            return 1
        try:
            did_change = import_into_reference(shot_path, reference_path)
        except ShotParseError as e:
            print(f"FAIL {reference_path.name}: {e}", file=sys.stderr)
            return 1
        changed += did_change
        print(
            f"OK {reference_path.name} <- {shot_path.name} ({'updated' if did_change else 'unchanged'})"
        )

    print(f"\n{len(pairs)} record(s) processed, {changed} changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
