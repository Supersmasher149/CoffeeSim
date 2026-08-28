#!/usr/bin/env python3
"""Black-box REST integration smoke test (Audit P1, issue #17).

apps/espressolab_server/main.cpp is not linked into espressolab_tests --
only the engine/library targets are (apps/espressolab_server/CMakeLists.txt
builds main.cpp straight into the espressolab_server binary), so request
translation, validation/error mapping, background job polling,
cancellation, retention, and artifact export have no Catch2 coverage at
all. Like the other tests/server/*.py and tests/cli/*.py scripts, this
starts the real built server as a subprocess on an isolated local port and
exercises the documented REST contract (docs/api.md) end to end:

    python3 tests/server/rest_integration_smoke.py [path/to/espressolab_server]

Coverage, per the issue's checklist:
  * health
  * valid and invalid shots (malformed JSON, missing field, out-of-range
    recipe) with the section 12.2 structured error contract
  * shot retrieval by id, and 404 for an unknown id
  * CSV export for a shot, a completed sweep, a still-running sweep
    (409 SWEEP_NOT_FINISHED), and an unknown id (404)
  * sweeps: queue, poll to completion, list, and a malformed sweep request
  * cancellation: a large sweep cancelled just after it's queued ends
    "cancelled" with fewer completed runs than its total
  * retention: RunStore's kMaxRetained (128) FIFO-evicts the oldest shot
    once a 129th is stored

Each check prints PASS/FAIL with a short diagnostic. The process exit code
is 0 only if every check passed.
"""

import copy
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

PORT = 18741


def default_binary():
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(here, "..", ".."))
    return os.path.join(
        repo_root, "build", "apps", "espressolab_server", "espressolab_server"
    )


def repo_root():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.abspath(os.path.join(here, "..", ".."))


def request(base_url, method, path, body=None, raw=False):
    data = None
    headers = {}
    if body is not None:
        data = (body if raw else json.dumps(body)).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(
        base_url + path, data=data, headers=headers, method=method
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as response:
            raw_bytes = response.read()
            content_type = response.headers.get("Content-Type", "")
            parsed = (
                json.loads(raw_bytes) if "json" in content_type else raw_bytes.decode()
            )
            return response.status, parsed
    except urllib.error.HTTPError as error:
        raw_bytes = error.read()
        content_type = error.headers.get("Content-Type", "") if error.headers else ""
        try:
            parsed = (
                json.loads(raw_bytes) if "json" in content_type else raw_bytes.decode()
            )
        except json.JSONDecodeError:
            parsed = raw_bytes.decode(errors="replace")
        return error.code, parsed


def get(base_url, path):
    return request(base_url, "GET", path)


def post(base_url, path, body=None, raw=None):
    if raw is not None:
        return request(base_url, "POST", path, body=raw, raw=True)
    return request(base_url, "POST", path, body=body)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else default_binary()
    if not os.path.isfile(binary):
        print(f"SKIP: binary not found at {binary} (build it with ./scripts/build.sh)")
        return 1

    root = repo_root()
    recipe_path = os.path.join(root, "assets", "recipes", "baseline.json")
    if not os.path.isfile(recipe_path):
        print(f"SKIP: fixture recipe not found at {recipe_path}")
        return 1
    with open(recipe_path) as f:
        baseline = json.load(f)

    base_url = f"http://127.0.0.1:{PORT}"
    proc = subprocess.Popen(
        [binary, "--port", str(PORT), "--assets", os.path.join(root, "assets")],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    checks = []

    def record(name, ok, detail=""):
        status = "PASS" if ok else "FAIL"
        print(f"{status}: {name}")
        if not ok and detail:
            print(f"  {detail}")
        checks.append(ok)

    try:
        for _ in range(50):
            try:
                get(base_url, "/api/v1/health")
                break
            except (urllib.error.URLError, ConnectionRefusedError):
                time.sleep(0.1)
        else:
            print("SKIP: server did not become healthy in time")
            return 1

        # -------------------------------------------------------- health --
        status, body = get(base_url, "/api/v1/health")
        ok = status == 200 and body.get("status") == "ok" and "solver_version" in body
        record(
            "GET /health reports ok with a solver version", ok, json.dumps(body)[:200]
        )

        # --------------------------------------------------------- shots --
        status, body = post(base_url, "/api/v1/shots", {"recipe": baseline})
        ok = (
            status == 201
            and "manifest" in body
            and bool(body["manifest"].get("run_id"))
        )
        record(
            "POST /shots with a valid recipe returns 201 and a manifest",
            ok,
            json.dumps(body)[:200],
        )
        shot_id = body.get("manifest", {}).get("run_id") if ok else None

        status, body = post(base_url, "/api/v1/shots", raw="{not json")
        ok = status == 400 and body.get("error", {}).get("code") == "MALFORMED_JSON"
        record(
            "POST /shots with malformed JSON returns 400 MALFORMED_JSON",
            ok,
            json.dumps(body)[:200],
        )

        status, body = post(base_url, "/api/v1/shots", {})
        ok = status == 400 and body.get("error", {}).get("code") == "MISSING_FIELD"
        record(
            "POST /shots with no recipe field returns 400 MISSING_FIELD",
            ok,
            json.dumps(body)[:200],
        )

        bad_recipe = copy.deepcopy(baseline)
        bad_recipe["puck"]["dose_g"] = 5.0  # below the 14-22 g range
        status, body = post(base_url, "/api/v1/shots", {"recipe": bad_recipe})
        issues = body.get("error", {}).get("details", {}).get("issues", [])
        ok = status == 422 and any(
            i.get("path") == "recipe.puck.dose_g" for i in issues
        )
        record(
            "POST /shots with an out-of-range recipe returns 422 with issues",
            ok,
            json.dumps(body)[:300],
        )

        if shot_id:
            status, body = get(base_url, f"/api/v1/shots/{shot_id}")
            ok = status == 200 and body.get("manifest", {}).get("run_id") == shot_id
            record(
                "GET /shots/{id} retrieves the stored shot", ok, json.dumps(body)[:200]
            )

        status, body = get(base_url, "/api/v1/shots/no-such-run")
        ok = status == 404 and body.get("error", {}).get("code") == "RUN_NOT_FOUND"
        record(
            "GET /shots/{unknown id} returns 404 RUN_NOT_FOUND",
            ok,
            json.dumps(body)[:200],
        )

        # ------------------------------------------------------ artifacts --
        if shot_id:
            status, body = get(base_url, f"/api/v1/artifacts/{shot_id}.csv")
            ok = status == 200 and isinstance(body, str) and body.count("\n") > 1
            record(
                "GET /artifacts/{shot}.csv returns a multi-row CSV", ok, str(body)[:150]
            )

        status, body = get(base_url, "/api/v1/artifacts/no-such-id.csv")
        ok = status == 404 and body.get("error", {}).get("code") == "ARTIFACT_NOT_FOUND"
        record(
            "GET /artifacts/{unknown id}.csv returns 404 ARTIFACT_NOT_FOUND",
            ok,
            json.dumps(body)[:200],
        )

        # --------------------------------------------------------- sweeps --
        small_sweep = {
            "name": "smoke-small",
            "baseline": baseline,
            "axes": [{"parameter_path": "puck.dose_g", "values": [16.0, 18.0, 20.0]}],
        }
        status, body = post(base_url, "/api/v1/sweeps", small_sweep)
        ok = status == 202 and body.get("sweep_id") and body.get("total") == 3
        record("POST /sweeps queues a small sweep (202)", ok, json.dumps(body)[:200])
        sweep_id = body.get("sweep_id") if ok else None

        if sweep_id:
            snapshot = {}
            for _ in range(100):
                _, snapshot = get(base_url, f"/api/v1/sweeps/{sweep_id}")
                if snapshot.get("status") == "complete":
                    break
                time.sleep(0.1)
            ok = snapshot.get("status") == "complete" and snapshot.get("completed") == 3
            record(
                "GET /sweeps/{id} polls through to complete",
                ok,
                json.dumps(snapshot)[:200],
            )

            status, body = get(base_url, "/api/v1/sweeps")
            ok = status == 200 and any(
                s.get("sweep_id") == sweep_id for s in body.get("sweeps", [])
            )
            record("GET /sweeps lists the queued sweep", ok, json.dumps(body)[:200])

            status, body = get(base_url, f"/api/v1/artifacts/{sweep_id}.csv")
            ok = status == 200 and isinstance(body, str) and body.count("\n") > 1
            record(
                "GET /artifacts/{completed sweep}.csv returns an aggregate CSV",
                ok,
                str(body)[:150],
            )

        status, body = post(base_url, "/api/v1/sweeps", {"baseline": baseline})
        ok = status == 422 and body.get("error", {}).get("code") == "EMPTY_SWEEP"
        record(
            "POST /sweeps with no axes returns 422 EMPTY_SWEEP",
            ok,
            json.dumps(body)[:200],
        )

        status, body = get(base_url, "/api/v1/sweeps/no-such-sweep")
        ok = status == 404 and body.get("error", {}).get("code") == "SWEEP_NOT_FOUND"
        record(
            "GET /sweeps/{unknown id} returns 404 SWEEP_NOT_FOUND",
            ok,
            json.dumps(body)[:200],
        )

        # ---------------------------------------------------- cancellation --
        # A run large enough that cancelling immediately after queuing it
        # still catches it mid-flight, so the check does not race the
        # background worker on a fast machine.
        big_sweep = {
            "name": "smoke-cancel",
            "baseline": baseline,
            "axes": [
                {
                    "parameter_path": "puck.dose_g",
                    "values": [14.0 + 0.05 * i for i in range(140)],
                },
                {
                    "parameter_path": "puck.particle_diameter_um",
                    "values": [150.0 + 5.0 * i for i in range(130)],
                },
            ],
        }
        status, body = post(base_url, "/api/v1/sweeps", big_sweep)
        ok = status == 202
        record(
            "POST /sweeps queues a large (18,200-run) sweep for cancellation",
            ok,
            json.dumps(body)[:200],
        )
        cancel_id = body.get("sweep_id") if ok else None

        if cancel_id:
            status, body = get(base_url, f"/api/v1/artifacts/{cancel_id}.csv")
            ok = (
                status == 409
                and body.get("error", {}).get("code") == "SWEEP_NOT_FINISHED"
            )
            record(
                "GET /artifacts/{still-running sweep}.csv returns 409 SWEEP_NOT_FINISHED",
                ok,
                json.dumps(body)[:200],
            )

            status, body = post(base_url, f"/api/v1/sweeps/{cancel_id}/cancel")
            ok = status == 200 and body.get("cancel_requested") is True
            record("POST /sweeps/{id}/cancel is accepted", ok, json.dumps(body)[:200])

            snapshot = {}
            for _ in range(100):
                _, snapshot = get(base_url, f"/api/v1/sweeps/{cancel_id}")
                if snapshot.get("status") in ("cancelled", "complete"):
                    break
                time.sleep(0.1)
            ok = snapshot.get("status") == "cancelled" and snapshot.get(
                "completed", snapshot.get("total", 0)
            ) < snapshot.get("total", 0)
            record(
                "cancelling mid-flight leaves the sweep 'cancelled' with a partial run count",
                ok,
                json.dumps(snapshot)[:200],
            )

        status, body = post(base_url, "/api/v1/sweeps/no-such-sweep/cancel")
        ok = status == 404 and body.get("error", {}).get("code") == "SWEEP_NOT_FOUND"
        record(
            "POST /sweeps/{unknown id}/cancel returns 404 SWEEP_NOT_FOUND",
            ok,
            json.dumps(body)[:200],
        )

        # ---------------------------------------------------------- retention --
        # RunStore::kMaxRetained is 128 (apps/espressolab_server/main.cpp):
        # storing a 129th shot must FIFO-evict the oldest one. run_id is the
        # first 12 hex chars of result_hash, deterministic over the recipe
        # (hashing.cpp) -- posting the same recipe 129 times would hash to
        # the same run_id every time and just overwrite one RunStore entry
        # in place, never growing past size 1. Vary dose_g so each post is a
        # genuinely different shot with its own run_id, and use a recipe
        # that finishes fast so 129 real POSTs stay quick.
        first_id = None
        retention_ok = True
        for i in range(129):
            fast_recipe = copy.deepcopy(baseline)
            fast_recipe["puck"]["dose_g"] = 14.0 + i * (22.0 - 14.0) / 128.0
            fast_recipe["stop"]["target_beverage_g"] = 20
            fast_recipe["stop"]["maximum_time_s"] = 15
            status, body = post(base_url, "/api/v1/shots", {"recipe": fast_recipe})
            if status != 201:
                retention_ok = False
                break
            run_id = body.get("manifest", {}).get("run_id")
            if i == 0:
                first_id = run_id

        if retention_ok and first_id:
            status, body = get(base_url, f"/api/v1/shots/{first_id}")
            ok = status == 404
            record(
                "storing a 129th shot evicts the oldest (kMaxRetained=128, FIFO)",
                ok,
                f"oldest shot {first_id} still resolves with status {status}"
                if not ok
                else "",
            )
        else:
            record(
                "storing a 129th shot evicts the oldest (kMaxRetained=128, FIFO)",
                False,
                "setup failed",
            )
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)

    if all(checks):
        print(f"\n{len(checks)}/{len(checks)} checks passed")
        return 0
    print(f"\n{sum(checks)}/{len(checks)} checks passed")
    return 1


if __name__ == "__main__":
    sys.exit(main())
