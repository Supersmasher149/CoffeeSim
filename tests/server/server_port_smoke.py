#!/usr/bin/env python3
"""Black-box --port argument smoke test (Audit F9, issue #10).

`port_from_args()` in `apps/espressolab_server/main.cpp` builds the actual
server executable; it is not linked into `espressolab_tests` (only the
engine/library targets are -- `apps/espressolab_server/CMakeLists.txt`
builds `main.cpp` straight into the `espressolab_server` binary). So, like
`tests/cli/cli_argv_smoke.py`, this exercises the real built binary as a
subprocess rather than a Catch2 unit test:

    python3 tests/server/server_port_smoke.py [path/to/espressolab_server]

Coverage: a non-numeric, negative, or out-of-range --port fails with a
clean nonzero exit and no abort/core-dump (previously an uncaught
std::invalid_argument/std::out_of_range from std::stoi() crashed the
process with exit code 134). A valid port still starts the server.

Each check prints PASS/FAIL with a short diagnostic. The process exit code
is 0 only if every check passed.
"""

import os
import signal
import subprocess
import sys
import time


def default_binary():
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(here, "..", ".."))
    return os.path.join(
        repo_root, "build", "apps", "espressolab_server", "espressolab_server"
    )


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else default_binary()
    if not os.path.isfile(binary):
        print(f"SKIP: binary not found at {binary} (build it with ./scripts/build.sh)")
        return 1

    checks = []

    def check_rejects_cleanly(name, port_value):
        result = subprocess.run(
            [binary, "--port", port_value], capture_output=True, text=True, timeout=10
        )
        # A crash (SIGABRT etc.) shows up as a negative returncode on POSIX;
        # exit code 134 is what std::abort() produced before this fix.
        clean = result.returncode > 0 and result.returncode != 134
        ok = clean and "INVALID_ARGUMENT" in result.stderr
        status = "PASS" if ok else "FAIL"
        print(
            f"{status}: --port {port_value!r} is rejected cleanly (exit={result.returncode})"
        )
        if not ok:
            print(f"  stderr: {result.stderr.strip()}")
        checks.append(ok)

    check_rejects_cleanly("non-numeric port", "nope")
    check_rejects_cleanly("negative port", "-5")
    check_rejects_cleanly("out-of-range port", "999999")
    check_rejects_cleanly("port too large to parse as int", "99999999999999999999")

    # A valid port still starts the server; kill it once it's had a chance
    # to bind rather than waiting for it to exit on its own.
    proc = subprocess.Popen(
        [binary, "--port", "18734"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.5)
    started = proc.poll() is None
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
    status = "PASS" if started else "FAIL"
    print(f"{status}: valid --port still starts the server")
    checks.append(started)

    if all(checks):
        print(f"\n{len(checks)}/{len(checks)} checks passed")
        return 0
    print(f"\n{sum(checks)}/{len(checks)} checks passed")
    return 1


if __name__ == "__main__":
    sys.exit(main())
