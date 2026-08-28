#!/usr/bin/env python3
"""Black-box CLI exit-code smoke test (Audit F3, issue #4).

`simulate`, `cfd`, and `cfd3d` printed the solver's termination reason but
always returned exit 0, even for `numerical_failure` or `invalid_state` --
automation had no way to tell a failed shot from a successful one without
parsing stdout. Like `tests/cli/cli_argv_smoke.py`, this exercises the real
built binary as a subprocess, since `main.cpp` isn't linked into
`espressolab_tests`:

    python3 tests/cli/cli_exit_codes_smoke.py [path/to/espressolab_cli]

Coverage: a config known to drive the 2D CFD solver to `invalid_state`
(an overlarge --dt causes a saturation-invariant violation; see
`tests/integration/test_cfd.cpp`'s "CFD rejects saturation overshoot in
strict mode") now exits nonzero instead of 0, and still prints
"termination     invalid_state" so the diagnosis stays visible. A normal
run still exits 0.

`simulate` and `cfd3d` share the exact same one-line fix
(`is_failure_termination(...) ? kSolverFailure : kOk`) but no legitimate
recipe/flag combination was found that drives either into a failure
termination without corrupting solver internals directly (as the Catch2
unit tests for those solvers do) -- see the issue #4 commit message. Only
the `cfd` case is exercised here; the other two commands are covered by
code review and the shared helper, not by an executed repro.

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

    def run(args):
        return subprocess.run(
            [binary] + args, capture_output=True, text=True, timeout=30
        )

    # An overlarge --dt on a small mesh drives the 2D CFD solver to a
    # saturation-invariant violation, i.e. invalid_state.
    failing = run(
        ["cfd", "--recipe", recipe, "--radial", "4", "--axial", "8", "--dt", "1.0"]
    )
    ok = failing.returncode != 0 and "termination     invalid_state" in failing.stdout
    print(
        f"{'PASS' if ok else 'FAIL'}: invalid_state CFD result exits nonzero (exit={failing.returncode})"
    )
    if not ok:
        print(f"  stdout: {failing.stdout.strip()}")
    checks.append(ok)

    succeeding = run(["cfd", "--recipe", recipe, "--radial", "2", "--axial", "2"])
    ok = (
        succeeding.returncode == 0
        and "termination     target_mass_reached" in succeeding.stdout
    )
    print(
        f"{'PASS' if ok else 'FAIL'}: a normal CFD run still exits 0 (exit={succeeding.returncode})"
    )
    if not ok:
        print(f"  stdout: {succeeding.stdout.strip()}")
    checks.append(ok)

    if all(checks):
        print(f"\n{len(checks)}/{len(checks)} checks passed")
        return 0
    print(f"\n{sum(checks)}/{len(checks)} checks passed")
    return 1


if __name__ == "__main__":
    sys.exit(main())
