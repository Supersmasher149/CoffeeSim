#!/usr/bin/env python3
"""Malformed-numeric-flag regression check for `espressolab_cli`.

`main.cpp`'s command_* handlers are only linked into the `espressolab_cli`
executable, not into `espressolab_cli_support`, so they are not reachable
from the Catch2 suite. This is deliberately not wired into `ctest` for the
same reason `tests/pty/tui_smoke.py` isn't -- it needs the real built
binary -- but is meant to be run by hand or in CI alongside it:

    python3 tests/cli/malformed_flags_check.py [path/to/espressolab_cli]

Regresses a bug where a malformed numeric flag (`--dt abc`, an unparsed
trailing suffix like `--dt 5xyz`, `--ring-capacity 0`, ...) threw a raw
std::invalid_argument/std::out_of_range out of the command handler, was
caught only by main()'s generic `catch (const std::exception&)`, and was
reported as `error INTERNAL_ERROR: ...` with exit code 1 -- as if the solver
itself had failed, rather than a usage error (exit code 2) naming the bad
flag.

Each check prints PASS/FAIL with a short diagnostic. The process exit code
is 0 only if every check passed.
"""

import os
import subprocess
import sys

USAGE_ERROR = 2


def default_binary():
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(here, "..", ".."))
    return os.path.join(
        repo_root, "build", "apps", "espressolab_cli", "espressolab_cli"
    )


def run(binary, args):
    return subprocess.run([binary, *args], capture_output=True, text=True, timeout=30)


def check(binary, name, args, expect_code):
    result = run(binary, args)
    stderr = result.stderr
    ok = result.returncode == expect_code and "INTERNAL_ERROR" not in stderr
    status = "PASS" if ok else "FAIL"
    print(f"{status}: {name} (exit={result.returncode}, expect={expect_code})")
    if not ok:
        print(f"      stderr: {stderr.strip()}")
    return ok


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else default_binary()
    if not os.path.isfile(binary):
        print(f"SKIP: built binary not found at {binary} -- run scripts/build.sh first")
        return 1

    recipe = "assets/recipes/baseline.json"
    sweep_spec = "assets/sweeps/grind-size.json"

    checks = [
        (
            "simulate --dt not-a-number",
            ["simulate", "--recipe", recipe, "--dt", "abc"],
            USAGE_ERROR,
        ),
        (
            "simulate --dt trailing garbage",
            ["simulate", "--recipe", recipe, "--dt", "5xyz"],
            USAGE_ERROR,
        ),
        (
            "synthesize --seed not-a-number",
            [
                "synthesize",
                "--recipe",
                recipe,
                "--out",
                "/tmp/malformed_flags_check.json",
                "--seed",
                "notanumber",
            ],
            USAGE_ERROR,
        ),
        (
            "sweep --ring-capacity 0",
            ["sweep", "--spec", sweep_spec, "--workers", "2", "--ring-capacity", "0"],
            USAGE_ERROR,
        ),
        (
            "bench --repeats not-a-number",
            ["bench", "--seconds", "15", "--repeats", "abc"],
            USAGE_ERROR,
        ),
        (
            "cfd3d --nx not-a-number",
            ["cfd3d", "--recipe", recipe, "--nx", "abc"],
            USAGE_ERROR,
        ),
    ]

    passed = all(
        check(binary, name, args, expect_code) for name, args, expect_code in checks
    )
    print("ALL PASS" if passed else "FAILURES ABOVE")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
