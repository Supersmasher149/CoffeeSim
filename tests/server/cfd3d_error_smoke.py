#!/usr/bin/env python3
"""Black-box CFD3D job error smoke test (Audit F8, issue #8).

POST /api/v1/cfd3d/runs queues the case with only shape/loader-level
validation done (Cfd3dSolver::run() validates mesh limits and recipe/
coefficient physics later, inside the background worker), so a
shape-valid but physically invalid document -- e.g. a dose outside the
recipe's supported range -- got 202 Accepted, then polling reported a
hardcoded CFD3D_FAILED with no code or path. Like the other
tests/server/*.py and tests/cli/*.py scripts, this exercises the real
built server as a subprocess, since apps/espressolab_server/main.cpp
isn't linked into espressolab_tests (there is no REST integration test
harness at all yet -- that is issue #17's separate, larger task):

    python3 tests/server/cfd3d_error_smoke.py [path/to/espressolab_server]

Coverage: an out-of-range recipe dose still gets 202 Accepted (loader-level
checks don't cover physical ranges), but polling the job now reports the
solver's actual OUT_OF_RANGE code and recipe.puck.dose_g path instead of a
generic CFD3D_FAILED. A normal case still completes.

Each check prints PASS/FAIL with a short diagnostic. The process exit code
is 0 only if every check passed.
"""

import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request


def default_binary():
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(here, "..", ".."))
    return os.path.join(
        repo_root, "build", "apps", "espressolab_server", "espressolab_server"
    )


def post(base_url, path, body):
    request = urllib.request.Request(
        base_url + path,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


def get(base_url, path):
    with urllib.request.urlopen(base_url + path, timeout=10) as response:
        return response.status, json.loads(response.read())


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else default_binary()
    if not os.path.isfile(binary):
        print(f"SKIP: binary not found at {binary} (build it with ./scripts/build.sh)")
        return 1

    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(here, "..", ".."))
    recipe_path = os.path.join(repo_root, "assets", "recipes", "baseline.json")
    if not os.path.isfile(recipe_path):
        print(f"SKIP: fixture recipe not found at {recipe_path}")
        return 1
    with open(recipe_path) as f:
        baseline_recipe = json.load(f)

    port = 18737
    base_url = f"http://127.0.0.1:{port}"
    proc = subprocess.Popen(
        [binary, "--port", str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    checks = []
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

        # Shape-valid, physically invalid: dose outside the 14-22 g range.
        bad_recipe = json.loads(json.dumps(baseline_recipe))
        bad_recipe["puck"]["dose_g"] = 5.0
        status, body = post(
            base_url,
            "/api/v1/cfd3d/runs",
            {"recipe": bad_recipe, "mesh": {"nx": 4, "ny": 4, "nz": 8}},
        )
        ok = status == 202 and "run_id" in body
        print(
            f"{'PASS' if ok else 'FAIL'}: invalid recipe is still queued (202) (status={status})"
        )
        checks.append(ok)
        run_id = body.get("run_id")

        if run_id is not None:
            job = {}
            for _ in range(50):
                _, job = get(base_url, f"/api/v1/cfd3d/runs/{run_id}")
                if job.get("status") == "failed":
                    break
                time.sleep(0.1)
            error = job.get("error", {})
            ok = (
                job.get("status") == "failed"
                and error.get("code") == "OUT_OF_RANGE"
                and error.get("path") == "recipe.puck.dose_g"
            )
            print(
                f"{'PASS' if ok else 'FAIL'}: polling reports the solver's structured error, not CFD3D_FAILED"
            )
            if not ok:
                print(f"  job: {json.dumps(job)}")
            checks.append(ok)

        # Two independent physical checks fail at once (dose and particle
        # diameter are unrelated fields validated separately, so both
        # OUT_OF_RANGE issues accumulate rather than short-circuiting).
        # Regression for issue #5: polling used to report only the first.
        doubly_bad_recipe = json.loads(json.dumps(baseline_recipe))
        doubly_bad_recipe["puck"]["dose_g"] = 5.0
        doubly_bad_recipe["puck"]["particle_diameter_um"] = 50.0
        status, body = post(
            base_url,
            "/api/v1/cfd3d/runs",
            {"recipe": doubly_bad_recipe, "mesh": {"nx": 4, "ny": 4, "nz": 8}},
        )
        run_id = body.get("run_id")
        job = {}
        if status == 202 and run_id is not None:
            for _ in range(50):
                _, job = get(base_url, f"/api/v1/cfd3d/runs/{run_id}")
                if job.get("status") == "failed":
                    break
                time.sleep(0.1)
        issues = job.get("error", {}).get("details", {}).get("issues", [])
        issue_paths = {issue.get("path") for issue in issues}
        ok = (
            status == 202
            and job.get("status") == "failed"
            and "recipe.puck.dose_g" in issue_paths
            and "recipe.puck.particle_diameter_um" in issue_paths
        )
        print(
            f"{'PASS' if ok else 'FAIL'}: polling reports every simultaneous validation issue, not just the first"
        )
        if not ok:
            print(f"  job: {json.dumps(job)}")
        checks.append(ok)

        # A normal case still completes.
        status, body = post(
            base_url,
            "/api/v1/cfd3d/runs",
            {"recipe": baseline_recipe, "mesh": {"nx": 2, "ny": 2, "nz": 4}},
        )
        run_id = body.get("run_id")
        job = {}
        if status == 202 and run_id is not None:
            for _ in range(100):
                _, job = get(base_url, f"/api/v1/cfd3d/runs/{run_id}")
                if job.get("status") in ("complete", "failed"):
                    break
                time.sleep(0.2)
        ok = status == 202 and job.get("status") == "complete"
        print(f"{'PASS' if ok else 'FAIL'}: a normal case still completes")
        if not ok:
            print(f"  job: {json.dumps(job)}")
        checks.append(ok)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)

    if all(checks):
        print(f"\n{len(checks)}/{len(checks)} checks passed")
        return 0
    print(f"\n{sum(checks)}/{len(checks)} checks passed")
    return 1


if __name__ == "__main__":
    sys.exit(main())
