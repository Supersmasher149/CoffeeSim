#!/usr/bin/env python3
"""Black-box sweep concurrency-limit smoke test.

SweepJobStore::start() used to spawn a worker thread for every POST
unconditionally -- kMaxRetained only bounded how many *finished* sweeps stay
in memory, not how many run concurrently, and a single sweep can be up to
20000 runs. Fixed the same way Cfd3dJobStore was fixed for Audit P2, issue
#13. Like the other tests/server/*.py scripts, this exercises the real built
server as a subprocess, since apps/espressolab_server/main.cpp isn't linked
into espressolab_tests:

    python3 tests/server/sweep_concurrency_smoke.py [path/to/espressolab_server]

Coverage: submitting 5 concurrent sweep requests sized to keep the solver
busy leaves at most kMaxConcurrent (4) running -- the rest get a stable,
synchronous 429 TOO_MANY_ACTIVE_RUNS response rather than an unbounded
number of worker threads.

Each check prints PASS/FAIL with a short diagnostic. The process exit code
is 0 only if every check passed.
"""

import json
import os
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request


def default_binary():
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(here, "..", ".."))
    return os.path.join(
        repo_root, "build", "apps", "espressolab_server", "espressolab_server"
    )


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
        recipe = json.load(f)

    port = 18740
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

        # Two axes within their schema ranges (150-800 um, 0.1-1.0 spread),
        # 2000 runs total, sized to keep each sweep's worker thread busy long
        # enough that all 5 requests land while the first 4 are still running.
        body = json.dumps(
            {
                "name": "concurrency-smoke",
                "baseline": recipe,
                "axes": [
                    {
                        "parameter_path": "puck.particle_diameter_um",
                        "values": [150 + i * (650 / 99) for i in range(100)],
                    },
                    {
                        "parameter_path": "puck.particle_spread_factor",
                        "values": [0.1 + i * (0.9 / 19) for i in range(20)],
                    },
                ],
            }
        ).encode()
        results = [None] * 5

        def fire(index):
            request = urllib.request.Request(
                base_url + "/api/v1/sweeps",
                data=body,
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            try:
                with urllib.request.urlopen(request, timeout=60) as response:
                    results[index] = (response.status, json.loads(response.read()))
            except urllib.error.HTTPError as error:
                results[index] = (error.code, json.loads(error.read()))

        threads = [threading.Thread(target=fire, args=(i,)) for i in range(5)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        accepted = [r for r in results if r is not None and r[0] == 202]
        rejected = [r for r in results if r is not None and r[0] == 429]

        ok = len(accepted) <= 4
        print(
            f"{'PASS' if ok else 'FAIL'}: no more than 4 concurrent sweeps accepted (accepted={len(accepted)})"
        )
        checks.append(ok)

        ok = len(rejected) >= 1 and all(
            r[1]["error"]["code"] == "TOO_MANY_ACTIVE_RUNS" for r in rejected
        )
        print(
            f"{'PASS' if ok else 'FAIL'}: the rest get a stable 429 TOO_MANY_ACTIVE_RUNS (rejected={len(rejected)})"
        )
        if not ok:
            print(f"  results: {results}")
        checks.append(ok)

        # Give the accepted jobs time to finish, then confirm a slot frees up.
        for sweep_id in [r[1]["sweep_id"] for r in accepted]:
            for _ in range(300):
                _, job = get(base_url, f"/api/v1/sweeps/{sweep_id}")
                if job.get("status") in ("complete", "cancelled", "failed"):
                    break
                time.sleep(0.2)

        request = urllib.request.Request(
            base_url + "/api/v1/sweeps",
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=30) as response:
            status = response.status
        ok = status == 202
        print(
            f"{'PASS' if ok else 'FAIL'}: the slot frees up once earlier sweeps finish (status={status})"
        )
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
