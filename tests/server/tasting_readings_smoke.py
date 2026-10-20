#!/usr/bin/env python3
"""Black-box checks for GET /api/v1/tasting-readings.

The route lists the sidecars `tools/jev_tasting_notes.py` writes beside
real-shot captures (`assets/real_shots/*.tasting.{jev,local}.json`) for the
dashboard's flavour panel. `apps/espressolab_server/main.cpp` is not linked
into `espressolab_tests`, so this starts the real built server against a
throwaway assets directory and checks:

  - the repo's own assets (no readings committed) list as an empty catalogue
  - a missing real_shots/ directory is an empty catalogue, not an error
  - valid Jev and local readings come back sorted by id, re-emitted with only
    the fields the dashboard draws (no raw `answers`)
  - a schema 1.0 reading without `backend` is reported as `typesafe`
  - an invalid sidecar is an error entry naming the field, and does not hide
    the valid ones
  - captures and other files in real_shots/ are not listed

    python3 tests/server/tasting_readings_smoke.py [path/to/espressolab_server]
"""

import copy
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

AXES = (
    "fruit",
    "acidity",
    "sweetness",
    "chocolate",
    "body",
    "bitterness",
    "astringency",
)


def repo_root():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def default_binary():
    return os.path.join(
        repo_root(), "build", "apps", "espressolab_server", "espressolab_server"
    )


def get(base_url, path):
    try:
        with urllib.request.urlopen(base_url + path, timeout=15) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        payload = error.read()
        return error.code, json.loads(payload) if payload else {}


def start_server(binary, assets, port):
    process = subprocess.Popen(
        [binary, "--assets", assets, "--port", str(port)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    base_url = f"http://127.0.0.1:{port}"
    for _ in range(60):
        try:
            if get(base_url, "/api/v1/health")[0] == 200:
                return process, base_url
        except (urllib.error.URLError, ConnectionRefusedError):
            pass
        time.sleep(0.1)
    stop_server(process)
    raise RuntimeError("server did not become healthy")


def stop_server(process):
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def check(checks, condition, message, details=None):
    print(f"{'PASS' if condition else 'FAIL'}: {message}")
    if not condition and details is not None:
        print(f"  {details}")
    checks.append(condition)


def reading(capture_id, backend="local"):
    return {
        "schema_version": "1.1",
        "kind": "jev_tasting_reading",
        "backend": backend,
        "capture_id": capture_id,
        "capture_sha256": "0" * 64,
        "request_sha256": "0" * 64,
        "model_requested": "jev-1.14.0",
        "model": "local:qwen2.5:7b-instruct" if backend == "local" else "jev-1.14.0",
        "retrieved_at": "2026-10-20T16:00:00Z",
        "min_confidence": 0.5,
        "usage": {"input_tokens": 100, "output_tokens": 16},
        "interpretation": "A model's reading of free-text tasting notes.",
        "axes": {
            axis: {
                "status": "read",
                "level": "moderate",
                "mentioned_probability": 0.9,
                "confidence": 0.8,
                "level_probabilities": {
                    "absent": 0,
                    "faint": 0,
                    "moderate": 1,
                    "pronounced": 0,
                    "dominant": 0,
                },
            }
            for axis in AXES
        },
        "verdict": {
            "status": "read",
            "value": "balanced",
            "confidence": 0.9,
            "probabilities": {},
        },
        "channeling_reported_probability": 0.1,
        "answers": {"raw": "not for the browser"},
    }


def write(directory, name, document):
    with open(os.path.join(directory, name), "w", encoding="utf-8") as handle:
        handle.write(document if isinstance(document, str) else json.dumps(document))


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else default_binary()
    if not os.path.isfile(binary):
        print(f"SKIP: binary not found at {binary} (build it with ./scripts/build.sh)")
        return 1

    checks = []
    workdir = tempfile.mkdtemp(prefix="espressolab-tasting-")
    try:
        process, base_url = start_server(
            binary, os.path.join(repo_root(), "assets"), 18741
        )
        try:
            status, body = get(base_url, "/api/v1/tasting-readings")
            check(
                checks,
                status == 200 and body.get("count") == 0 and body.get("readings") == [],
                "the repo's assets list an empty tasting catalogue",
                body,
            )
        finally:
            stop_server(process)

        empty_assets = os.path.join(workdir, "empty")
        os.makedirs(empty_assets)
        process, base_url = start_server(binary, empty_assets, 18742)
        try:
            status, body = get(base_url, "/api/v1/tasting-readings")
            check(
                checks,
                status == 200 and body.get("count") == 0,
                "a missing real_shots/ directory is an empty catalogue",
                body,
            )
        finally:
            stop_server(process)

        assets = os.path.join(workdir, "assets")
        real_shots = os.path.join(assets, "real_shots")
        os.makedirs(real_shots)
        write(real_shots, "b-shot.tasting.local.json", reading("b-shot"))
        write(
            real_shots, "a-shot.tasting.jev.json", reading("a-shot", backend="typesafe")
        )
        legacy = reading("c-shot")
        del legacy["backend"]
        write(real_shots, "c-shot.tasting.jev.json", legacy)
        bad_level = copy.deepcopy(reading("d-shot"))
        bad_level["axes"]["body"]["level"] = "huge"
        write(real_shots, "d-shot.tasting.local.json", bad_level)
        write(real_shots, "e-shot.tasting.local.json", "{not json")
        write(real_shots, "a-shot.capture.json", {"kind": "real_shot_capture"})
        write(real_shots, "notes.json", reading("not-a-sidecar"))

        process, base_url = start_server(binary, assets, 18743)
        try:
            status, body = get(base_url, "/api/v1/tasting-readings")
            entries = body.get("readings", [])
            ids = [entry.get("id") for entry in entries]
            check(checks, status == 200, "catalogue returns 200", body)
            check(
                checks,
                ids
                == [
                    "a-shot.tasting.jev",
                    "b-shot.tasting.local",
                    "c-shot.tasting.jev",
                    "d-shot.tasting.local",
                    "e-shot.tasting.local",
                ],
                "only sidecars are listed, sorted by id",
                ids,
            )
            check(
                checks,
                body.get("count") == 5,
                "count matches the entries",
                body.get("count"),
            )

            by_id = {entry["id"]: entry for entry in entries}
            jev = by_id.get("a-shot.tasting.jev", {})
            check(
                checks,
                jev.get("backend") == "typesafe" and jev.get("model") == "jev-1.14.0",
                "a Jev reading keeps its backend and model",
                jev,
            )
            check(
                checks,
                set(jev.get("axes", {})) == set(AXES)
                and jev["axes"]["body"]
                == {"level": "moderate", "status": "read", "confidence": 0.8},
                "axes carry only level, status and confidence",
                jev.get("axes"),
            )
            check(
                checks,
                "answers" not in jev and "usage" not in jev,
                "raw answers and usage stay out of the browser",
                sorted(jev),
            )
            check(
                checks,
                by_id.get("c-shot.tasting.jev", {}).get("backend") == "typesafe",
                "a schema 1.0 reading without backend reads as typesafe",
                by_id.get("c-shot.tasting.jev"),
            )

            bad = by_id.get("d-shot.tasting.local", {})
            check(
                checks,
                bad.get("error", {}).get("code") == "TASTING_READING_INVALID"
                and "reading.axes.body.level" in bad["error"].get("message", ""),
                "an unknown level is an error entry naming the field",
                bad,
            )
            broken = by_id.get("e-shot.tasting.local", {})
            check(
                checks,
                broken.get("error", {}).get("code") == "TASTING_READING_INVALID",
                "malformed JSON is an error entry",
                broken,
            )
        finally:
            stop_server(process)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    failed = checks.count(False)
    print(f"\n{len(checks) - failed}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
