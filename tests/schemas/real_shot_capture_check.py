#!/usr/bin/env python3
"""Contract check for real-shot capture records (assets/real_shots/*.capture.json).

The point of a capture record is that it does not lie about what was measured,
so this checks the rules that a JSON Schema cannot express and that a careless
capture would silently break:

  - every field present, missing values held as null (never "", "TBD", "n/a")
  - a pending_capture record contains no observations at all
  - a grinder dial setting stays a string; PSD numbers require a measurement
  - a temperature setpoint is never presented as a measured temperature
  - pump pressure and puck pressure are distinguished explicitly
  - a computed extraction yield actually equals tds * yield / dose
  - telemetry columns and time origin match the target contract and timing
  - capture records never leak into assets/measured_shots/, which
    `espressolab_cli calibrate` reads wholesale

Stdlib only (unlike tests/schemas/schema_contract_check.py, which needs the
unvendored `jsonschema` package), so CI can run it:

    python3 tests/schemas/real_shot_capture_check.py
"""

import json
import sys
from pathlib import Path

TELEMETRY_COLUMNS = [
    "time_s",
    "pressure_bar",
    "flow_ml_s",
    "cumulative_beverage_mass_g",
    "temperature_c",
]

# A capture record says "not measured" with null. These strings are the usual
# ways a hand-filled form pretends to carry a value instead.
PLACEHOLDERS = {"", "tbd", "todo", "n/a", "na", "none", "unknown", "?", "-", "null"}

REQUIRED = {
    None: ["schema_version", "kind", "id", "status", "capture", "provenance", "machine",
           "grinder", "grind", "coffee", "preparation", "water", "temperature", "pressure",
           "timing", "result", "telemetry", "observations", "attachments", "simulation_link"],
    "capture": ["utc_timestamp", "operator", "protocol_version"],
    "provenance": ["source", "source_url", "license", "retrieved_at", "data_quality_flags",
                   "calibration_eligible", "validation_eligible"],
    "machine": ["model", "firmware", "basket", "portafilter_diameter_mm",
                "pressure_sensor_measures", "pressure_sensor_location",
                "temperature_sensor_location"],
    "grinder": ["model", "burrs", "dial_setting", "rpm"],
    "grind": ["measured", "method", "d10_um", "d50_um", "d90_um", "sauter_mean_d32_um",
              "distribution"],
    "coffee": ["roaster", "name", "origin", "process", "roast_level", "roast_date",
               "days_off_roast", "storage"],
    "preparation": ["dose_g", "dose_scale_resolution_g", "basket", "distribution_technique",
                    "tamp", "filter_paper", "puck_screen", "puck_depth_mm"],
    "water": ["recipe_name", "source", "tds_ppm", "general_hardness_ppm_caco3",
              "alkalinity_ppm_caco3"],
    "temperature": ["setpoint_c", "measured_c", "measurement_point", "measurement_method", "note"],
    "pressure": ["profile_name", "profile_description", "nominal_peak_bar", "preinfusion"],
    "timing": ["convention", "first_drip_s", "end_condition", "total_shot_time_s"],
    "result": ["beverage_yield_g", "yield_scale_resolution_g", "tds_percent", "tds_method",
               "extraction_yield_percent", "extraction_yield_source"],
    "telemetry": ["csv", "columns", "sample_rate_hz", "row_count", "time_origin"],
    "observations": ["taste", "channeling", "puck_condition", "anomalies", "notes"],
    "attachments": ["raw_shot_file", "telemetry_csv", "photos", "video"],
    "simulation_link": ["recipe", "unavailable_model_inputs", "comparable_observables"],
}

CURVE_OBSERVABLES = {"pressure_curve", "flow_curve", "temperature_curve",
                     "cumulative_mass_curve"}


def check_record(path: Path, record: dict) -> list[str]:
    errors: list[str] = []

    def fail(message: str):
        errors.append(f"{path.name}: {message}")

    for section, keys in REQUIRED.items():
        node = record if section is None else record.get(section)
        if not isinstance(node, dict):
            fail(f"{section or 'document'} must be an object")
            continue
        for key in keys:
            if key not in node:
                fail(f"missing field {section + '.' if section else ''}{key}")

    if errors:
        return errors  # Later rules assume the shape is there.

    if record["schema_version"] != "1.0":
        fail(f"schema_version {record['schema_version']!r} is not supported (expected '1.0')")
    if record["kind"] != "real_shot_capture":
        fail(f"kind must be 'real_shot_capture', got {record['kind']!r}")
    status = record["status"]
    if status not in ("measured", "external_public", "pending_capture"):
        fail(f"status {status!r} is not one of measured/external_public/pending_capture")
    if record["id"] != path.name.removesuffix(".capture.json"):
        fail(f"id {record['id']!r} does not match the filename")

    # Missing means null. A placeholder string is a fabricated value that reads
    # as data downstream.
    def walk(node, trail):
        if isinstance(node, dict):
            for key, value in node.items():
                walk(value, f"{trail}.{key}" if trail else key)
        elif isinstance(node, list):
            for i, value in enumerate(node):
                walk(value, f"{trail}[{i}]")
        elif isinstance(node, str) and node.strip().lower() in PLACEHOLDERS:
            fail(f"{trail} is the placeholder {node!r}; record an unmeasured value as null")

    walk(record, "")

    provenance = record["provenance"]
    if status != "measured" and provenance["calibration_eligible"]:
        fail(f"calibration_eligible must be false while status is {status!r}")
    if status == "pending_capture" and provenance["validation_eligible"]:
        fail("validation_eligible must be false until the shot is actually brewed")

    # A dial number is not a length. Keeping it typed as text is what stops it
    # from being read as microns.
    dial = record["grinder"]["dial_setting"]
    if dial is not None and not isinstance(dial, str):
        fail("grinder.dial_setting must be a string; a dial number is not a particle size")

    grind = record["grind"]
    psd_numbers = ["d10_um", "d50_um", "d90_um", "sauter_mean_d32_um"]
    has_psd = any(grind[key] is not None for key in psd_numbers) or bool(grind["distribution"])
    if has_psd and not grind["measured"]:
        fail("grind carries particle sizes but grind.measured is false; PSD is never derived "
             "from a grinder dial setting")
    if grind["measured"] and grind["method"] is None:
        fail("grind.measured is true but grind.method does not say how it was measured")
    total = sum(bin_["mass_fraction"] for bin_ in grind["distribution"])
    if grind["distribution"] and abs(total - 1.0) > 1e-6:
        fail(f"grind.distribution mass fractions sum to {total}, not 1")

    temperature = record["temperature"]
    if temperature["measured_c"] is not None:
        if temperature["measurement_point"] is None or temperature["measurement_method"] is None:
            fail("temperature.measured_c requires measurement_point and measurement_method; a "
                 "boiler setpoint is not a measured brew temperature")
        if temperature["setpoint_c"] is not None and record["status"] == "pending_capture":
            fail("a pending_capture record must not carry a measured temperature")

    sensor = record["machine"]["pressure_sensor_measures"]
    if sensor not in ("pump", "group", "puck", "unknown", None):
        fail(f"machine.pressure_sensor_measures {sensor!r} must be pump/group/puck/unknown/null")
    if record["telemetry"]["csv"] is not None and sensor is None:
        fail("telemetry carries a pressure column but machine.pressure_sensor_measures is null; "
             "pump pressure is not puck pressure")

    result = record["result"]
    dose = record["preparation"]["dose_g"]
    tds = result["tds_percent"]
    yield_g = result["beverage_yield_g"]
    ey = result["extraction_yield_percent"]
    if ey is not None and result["extraction_yield_source"] is None:
        fail("extraction_yield_percent requires extraction_yield_source (computed or measured)")
    if result["extraction_yield_source"] == "computed":
        if None in (dose, tds, yield_g, ey):
            fail("a computed extraction yield needs dose_g, tds_percent, beverage_yield_g and "
                 "extraction_yield_percent")
        else:
            expected = tds * yield_g / dose
            if abs(expected - ey) > 0.05:
                fail(f"extraction_yield_percent {ey} does not match tds*yield/dose "
                     f"({expected:.3f}) within 0.05 percentage points")
    readings = result["tds_method"]["readings_percent"]
    repeats = result["tds_method"]["repeats"]
    if readings and repeats is not None and len(readings) != repeats:
        fail(f"tds_method has {len(readings)} readings but claims {repeats} repeats")
    if tds is not None and not readings:
        fail("tds_percent is recorded but tds_method.readings_percent is empty; keep every reading")

    telemetry = record["telemetry"]
    if telemetry["csv"] is not None:
        if telemetry["columns"] != TELEMETRY_COLUMNS:
            fail(f"telemetry.columns must be exactly {TELEMETRY_COLUMNS}")
        if telemetry["time_origin"] != record["timing"]["convention"]:
            fail("telemetry.time_origin must equal timing.convention; a curve compared across "
                 "two different t=0 conventions is not time-aligned")
        csv_path = (path.parent / telemetry["csv"]).resolve()
        if not csv_path.is_file():
            fail(f"telemetry.csv points at missing file {telemetry['csv']}")
        else:
            header = csv_path.read_text().splitlines()[0].strip()
            if header != ",".join(TELEMETRY_COLUMNS):
                fail(f"{telemetry['csv']} header is {header!r}, expected "
                     f"{','.join(TELEMETRY_COLUMNS)}")
        if telemetry["csv"] != record["attachments"]["telemetry_csv"]:
            fail("attachments.telemetry_csv must repeat telemetry.csv")
    elif telemetry["columns"] or telemetry["row_count"]:
        fail("telemetry has columns or rows but no csv file")

    link = record["simulation_link"]
    observables = link["comparable_observables"]
    if link["recipe"] is None and observables:
        fail("comparable_observables is non-empty but no recipe reproduces this shot's inputs")
    if link["unavailable_model_inputs"] and link["recipe"] is not None:
        fail("simulation_link names unavailable model inputs, so recipe must stay null rather "
             "than invent them")
    if CURVE_OBSERVABLES & set(observables) and telemetry["csv"] is None:
        fail("a curve observable requires a time-aligned telemetry CSV")

    if status == "pending_capture":
        observed = {
            "capture.utc_timestamp": record["capture"]["utc_timestamp"],
            "preparation.dose_g": record["preparation"]["dose_g"],
            "timing.total_shot_time_s": record["timing"]["total_shot_time_s"],
            "timing.first_drip_s": record["timing"]["first_drip_s"],
            "result.beverage_yield_g": yield_g,
            "result.tds_percent": tds,
            "result.extraction_yield_percent": ey,
            "temperature.measured_c": temperature["measured_c"],
            "telemetry.csv": telemetry["csv"],
        }
        for field, value in observed.items():
            if value is not None:
                fail(f"status is pending_capture but {field} carries a value ({value!r}); no "
                     "shot has been brewed")

    return errors


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    capture_dir = root / "assets" / "real_shots"

    records = sorted(path for path in capture_dir.glob("*.capture.json")
                     if path.name != "TEMPLATE.capture.json")
    if not records:
        print(f"FAIL: no capture records found in {capture_dir}")
        return 1
    records.append(capture_dir / "TEMPLATE.capture.json")

    errors: list[str] = []
    for path in records:
        try:
            record = json.loads(path.read_text())
        except json.JSONDecodeError as e:
            errors.append(f"{path.name}: not valid JSON ({e})")
            continue
        if path.name == "TEMPLATE.capture.json":
            # The template is checked for shape, not for its placeholder id.
            record = dict(record, id=path.name.removesuffix(".capture.json"))
        errors.extend(check_record(path, record))

    template_header = (capture_dir / "TEMPLATE.telemetry.csv").read_text().splitlines()[0].strip()
    if template_header != ",".join(TELEMETRY_COLUMNS):
        errors.append(f"TEMPLATE.telemetry.csv header is {template_header!r}")

    # `espressolab_cli calibrate` reads every .json in its directory as a
    # measured shot, so a capture record dropped there would break the fit.
    for stray in (root / "assets" / "measured_shots").glob("*.capture.json"):
        errors.append(f"{stray.name}: capture records must not live in assets/measured_shots/")

    for error in errors:
        print(f"FAIL {error}")
    checked = len(records)
    if errors:
        print(f"\n{len(errors)} problem(s) across {checked} capture record(s)")
        return 1
    print(f"OK: {checked} capture record(s) satisfy the real-shot capture contract")
    return 0


if __name__ == "__main__":
    sys.exit(main())
