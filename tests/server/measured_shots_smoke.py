#!/usr/bin/env python3
"""Black-box measured-shot catalogue and comparison API checks."""

import json
import math
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request


def repo_root():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def default_binary():
    return os.path.join(repo_root(), "build", "apps", "espressolab_server", "espressolab_server")


def get(base_url, path):
    try:
        with urllib.request.urlopen(base_url + path, timeout=15) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        payload = error.read()
        return error.code, json.loads(payload) if payload else {}


def wait_for_server(base_url):
    for _ in range(60):
        try:
            status, _ = get(base_url, "/api/v1/health")
            if status == 200:
                return True
        except (urllib.error.URLError, ConnectionRefusedError):
            pass
        time.sleep(0.1)
    return False


def start_server(binary, assets, port):
    process = subprocess.Popen(
        [binary, "--assets", assets, "--port", str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    base_url = f"http://127.0.0.1:{port}"
    if not wait_for_server(base_url):
        process.terminate()
        process.wait(timeout=5)
        raise RuntimeError("server did not become healthy")
    return process, base_url


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


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else default_binary()
    if not os.path.isfile(binary):
        print(f"SKIP: binary not found at {binary} (build it with ./scripts/build.sh)")
        return 1

    checks = []
    process, base_url = start_server(binary, os.path.join(repo_root(), "assets"), 18738)
    try:
        status, catalogue = get(base_url, "/api/v1/measured-shots")
        shots = catalogue.get("measured_shots", [])
        ids = [shot.get("id") for shot in shots]
        check(checks, status == 200, "catalogue returns 200", catalogue)
        check(checks, catalogue.get("count") == 3 and len(shots) == 3,
              "catalogue contains all three measured-shot fixtures", catalogue)
        check(checks, ids == sorted(ids), "catalogue is sorted by embedded id", ids)
        check(checks, all(shot.get("synthetic") is True for shot in shots),
              "synthetic flags pass through unchanged", shots)
        check(checks, all("source_stem" in shot and "final" in shot for shot in shots),
              "catalogue exposes source stems and nullable final objects", shots)

        selected = shots[0]
        identifier = urllib.parse.quote(selected["id"], safe="")
        status, comparison = get(
            base_url, f"/api/v1/measured-shots/{identifier}/compare?coefficients=default-v1"
        )
        check(checks, status == 200, "comparison by embedded id returns 200", comparison)
        coefficient = comparison.get("coefficients", {})
        check(
            checks,
            coefficient.get("selector") == "default-v1"
            and coefficient.get("id") == "default"
            and coefficient.get("version") == "1.0.0"
            and len(coefficient.get("hash", "")) == 64,
            "comparison reports selector and exact coefficient identity",
            coefficient,
        )
        pairs = comparison.get("paired_series", [])
        residuals_match = bool(pairs) and all(
            math.isclose(
                pair["residual_g"],
                pair["measured_mass_g"] - pair["simulated_mass_g"],
                rel_tol=0.0,
                abs_tol=1.0e-12,
            )
            for pair in pairs
        )
        check(checks, residuals_match, "paired residuals are measured minus simulated")
        loss = comparison.get("loss", {})
        check(
            checks,
            all(key in loss for key in (
                "mass_rmse_g", "time_error_s", "tds_error_percent", "pressure_rmse_bar",
                "regularization", "total", "simulated", "has_time_measurement",
                "has_tds_measurement", "has_pressure_measurement",
            )),
            "comparison returns the complete native loss breakdown",
            loss,
        )

        stem = urllib.parse.quote(selected["source_stem"], safe="")
        stem_status, stem_comparison = get(
            base_url, f"/api/v1/measured-shots/{stem}/compare"
        )
        check(
            checks,
            stem_status == 200 and stem_comparison.get("id") == selected["id"],
            "comparison lookup accepts the source stem alias",
            stem_comparison,
        )
        repeat_status, repeated = get(
            base_url, f"/api/v1/measured-shots/{identifier}/compare"
        )
        check(
            checks,
            repeat_status == 200
            and repeated.get("simulation", {}).get("result_hash")
            == comparison.get("simulation", {}).get("result_hash")
            and repeated.get("loss") == comparison.get("loss"),
            "repeated comparison preserves result identity and metrics",
            repeated,
        )

        status, body = get(base_url, "/api/v1/measured-shots/unknown/compare")
        check(checks, status == 404 and body.get("error", {}).get("code") == "MEASURED_SHOT_NOT_FOUND",
              "unknown measured shot returns structured 404", body)
        status, body = get(
            base_url,
            f"/api/v1/measured-shots/{identifier}/compare?coefficients=unknown",
        )
        check(checks, status == 404 and body.get("error", {}).get("code") == "COEFFICIENTS_NOT_FOUND",
              "unknown coefficient selector returns structured 404", body)
        status, _ = get(base_url, "/api/v1/measured-shots/%2e%2e%2frecipes%2fbaseline/compare")
        check(checks, status != 200, "encoded path traversal cannot select an asset")
    finally:
        stop_server(process)

    with tempfile.TemporaryDirectory(prefix="espressolab-measured-api-") as temporary:
        copied_assets = os.path.join(temporary, "assets")
        shutil.copytree(os.path.join(repo_root(), "assets"), copied_assets)
        malformed = os.path.join(copied_assets, "measured_shots", "malformed.json")
        with open(malformed, "w", encoding="utf-8") as stream:
            json.dump({"schema_version": "1.0", "recipe": 42}, stream)
        process, base_url = start_server(binary, copied_assets, 18739)
        try:
            status, body = get(base_url, "/api/v1/measured-shots")
            check(
                checks,
                status == 500 and body.get("error", {}).get("code") == "MEASURED_SHOT_LOAD_FAILED",
                "malformed stored shot fails the whole catalogue structurally",
                body,
            )
        finally:
            stop_server(process)

    with tempfile.TemporaryDirectory(prefix="espressolab-measured-api-") as temporary:
        copied_assets = os.path.join(temporary, "assets")
        shutil.copytree(os.path.join(repo_root(), "assets"), copied_assets)
        measured = os.path.join(copied_assets, "measured_shots")
        source = sorted(name for name in os.listdir(measured) if name.endswith(".json"))[0]
        with open(os.path.join(measured, source), encoding="utf-8") as stream:
            duplicate = json.load(stream)
        duplicate["id"] = os.path.splitext(source)[0]
        with open(os.path.join(measured, "alias-collision.json"), "w", encoding="utf-8") as stream:
            json.dump(duplicate, stream)
        process, base_url = start_server(binary, copied_assets, 18740)
        try:
            status, body = get(base_url, "/api/v1/measured-shots")
            check(
                checks,
                status == 500 and body.get("error", {}).get("code") == "MEASURED_SHOT_LOAD_FAILED",
                "ambiguous id/source-stem aliases reject the catalogue",
                body,
            )
        finally:
            stop_server(process)

    print(f"\n{sum(checks)}/{len(checks)} checks passed")
    return 0 if all(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
