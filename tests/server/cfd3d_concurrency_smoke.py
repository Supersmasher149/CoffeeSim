#!/usr/bin/env python3
"""Black-box CFD3D concurrency-limit smoke test (Audit P2, issue #13).

Cfd3dJobStore::start() used to spawn a worker thread for every POST
unconditionally -- kMaxRetained only bounded how many *finished* jobs stay
in memory, not how many run concurrently, and each is a real 3D
finite-volume solve that can retain up to 1 GiB of snapshots. Like the
other tests/server/*.py scripts, this exercises the real built server as
a subprocess, since apps/espressolab_server/main.cpp isn't linked into
espressolab_tests:

    python3 tests/server/cfd3d_concurrency_smoke.py [path/to/espressolab_server]

Coverage: submitting 3 concurrent CFD3D requests against a mesh sized to
keep the solver busy leaves at most kMaxConcurrent (2) running -- the
others get a stable, synchronous 429 TOO_MANY_ACTIVE_RUNS response rather
than an unbounded number of worker threads.

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

    port = 18739
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

        # Sized to keep the solver busy for long enough that all 3 requests
        # land while the first two are still running.
        body = json.dumps(
            {"recipe": recipe, "mesh": {"nx": 16, "ny": 16, "nz": 24}}
        ).encode()
        results = [None, None, None]

        def fire(index):
            request = urllib.request.Request(
                base_url + "/api/v1/cfd3d/runs",
                data=body,
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            try:
                with urllib.request.urlopen(request, timeout=30) as response:
                    results[index] = (response.status, json.loads(response.read()))
            except urllib.error.HTTPError as error:
                results[index] = (error.code, json.loads(error.read()))

        threads = [threading.Thread(target=fire, args=(i,)) for i in range(3)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        accepted = [r for r in results if r is not None and r[0] == 202]
        rejected = [r for r in results if r is not None and r[0] == 429]

        ok = len(accepted) <= 2
        print(
            f"{'PASS' if ok else 'FAIL'}: no more than 2 concurrent runs accepted (accepted={len(accepted)})"
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

        # Give the two accepted jobs time to finish, then confirm the slot
        # frees up again.
        for run_id in [r[1]["run_id"] for r in accepted]:
            for _ in range(150):
                _, job = get(base_url, f"/api/v1/cfd3d/runs/{run_id}")
                if job.get("status") in ("complete", "failed"):
                    break
                time.sleep(0.2)

        request = urllib.request.Request(
            base_url + "/api/v1/cfd3d/runs",
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=10) as response:
            status = response.status
        ok = status == 202
        print(
            f"{'PASS' if ok else 'FAIL'}: the slot frees up once earlier runs finish (status={status})"
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
