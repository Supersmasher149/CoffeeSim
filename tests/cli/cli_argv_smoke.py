#!/usr/bin/env python3
"""Black-box CLI argument-parsing smoke test (Audit F10, issue #11).

`parse_flags()` in `apps/espressolab_cli/main.cpp` builds the actual
executable; it is not linked into `espressolab_tests` (see
`apps/espressolab_cli/CMakeLists.txt`: only `espressolab_cli_support` is,
and the architecture in CLAUDE.md deliberately keeps argv parsing out of
that shared library). So, like `tests/pty/tui_smoke.py` for the TUI, this
exercises the real built binary as a subprocess rather than a Catch2 unit
test, and is meant to be run by hand or in CI alongside `ctest`:

    python3 tests/cli/cli_argv_smoke.py [path/to/espressolab_cli]

Coverage:
  * an unknown option (e.g. a typo like `--coefficient`) fails loudly with
    a nonzero exit code instead of silently running with defaults
  * an unexpected positional argument is rejected
  * a duplicate option is rejected
  * legitimate usage is unaffected (still exits 0)

Each check prints PASS/FAIL with a short diagnostic. The process exit code
is 0 only if every check passed.
"""

import os
import subprocess
import sys


def default_binary():
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(here, "..", ".."))
    return os.path.join(
        repo_root, "build", "apps", "espressolab_cli", "espressolab_cli"
    )


def run(binary, args):
    return subprocess.run([binary] + args, capture_output=True, text=True, timeout=30)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else default_binary()
    if not os.path.isfile(binary):
        print(f"SKIP: binary not found at {binary} (build it with ./scripts/build.sh)")
        return 1

    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(here, "..", ".."))
    recipe = os.path.join(repo_root, "assets", "recipes", "baseline.json")
    if not os.path.isfile(recipe):
        print(f"SKIP: fixture recipe not found at {recipe}")
        return 1

    checks = []

    def check(name, args, want_zero_exit, needle=None):
        result = run(binary, args)
        ok = (result.returncode == 0) == want_zero_exit
        if ok and needle is not None:
            ok = needle in result.stderr
        status = "PASS" if ok else "FAIL"
        print(f"{status}: {name} (exit={result.returncode})")
        if not ok:
            print(f"  stderr: {result.stderr.strip()}")
        checks.append(ok)

    check(
        "unknown option is rejected",
        ["simulate", "--recipe", recipe, "--quiet", "--unknown-option"],
        want_zero_exit=False,
        needle="UNKNOWN_OPTION",
    )
    check(
        "typo'd option is rejected rather than silently defaulted",
        ["simulate", "--recipe", recipe, "--coefficient", recipe, "--quiet"],
        want_zero_exit=False,
        needle="UNKNOWN_OPTION",
    )
    check(
        "unexpected positional argument is rejected",
        ["simulate", "--recipe", recipe, "extra-positional", "--quiet"],
        want_zero_exit=False,
        needle="UNEXPECTED_ARGUMENT",
    )
    check(
        "duplicate option is rejected",
        ["simulate", "--recipe", recipe, "--recipe", recipe, "--quiet"],
        want_zero_exit=False,
        needle="DUPLICATE_OPTION",
    )
    check(
        "legitimate usage still succeeds",
        ["simulate", "--recipe", recipe, "--quiet"],
        want_zero_exit=True,
    )

    if all(checks):
        print(f"\n{len(checks)}/{len(checks)} checks passed")
        return 0
    print(f"\n{sum(checks)}/{len(checks)} checks passed")
    return 1


if __name__ == "__main__":
    sys.exit(main())
