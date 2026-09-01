#!/usr/bin/env python3
"""POSIX PTY smoke matrix for `espressolab_cli tui` (issue #30).

This exercises what a Catch2 unit test cannot: a real pseudo-terminal in
front of the actual FTXUI render loop. It is deliberately not wired into
`ctest` -- CLAUDE.md's native suite stays terminal-free -- but is meant to be
run by hand or in a POSIX CI job alongside it:

    python3 tests/pty/tui_smoke.py [path/to/espressolab_cli]

Coverage, matching the issue's acceptance criteria:
  * launch on an interactive PTY and render the home screen
  * a guided command with form fields (`simulate`) can actually be run: Tab
    to the trailing Run action and Enter it, producing the same native
    result the file-oriented command prints (this exact path -- reaching
    Run past a non-empty field list -- once regressed silently because
    every other guided-command check here used a zero-field command)
  * a representative guided command (`version`, which needs no fields) runs
    and shows the same native output the file-oriented command prints
  * a form taller than the terminal (`cfd3d`, 11 fields) scrolls as you
    navigate past the bottom of a short PTY, so the trailing Run action is
    always reachable rather than rendering past the edge of the screen
  * resize (SIGWINCH) does not crash the app and it keeps responding
  * Ctrl-C from the menu exits cleanly
  * quitting restores the terminal (leaves the alternate screen, shows the
    cursor) and returns exit code 0
  * invoking `tui` with stdin/stdout redirected away from a terminal fails
    fast with a stable nonzero exit code and no PTY involved at all

Each check prints PASS/FAIL with a short diagnostic. The process exit code is
0 only if every check passed, and prerequisites (a POSIX PTY, the built
binary) are checked explicitly rather than assumed, per issue #30: "PTY tests
... fail with diagnostics when prerequisites are absent."
"""

import fcntl
import os
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time

try:
    import pty
except ImportError:
    print("SKIP: the `pty` module is not available on this platform (POSIX only)")
    sys.exit(1)


ESCAPE_RE = re.compile(r"\x1b\[[0-9;?]*[a-zA-Z]")


def default_binary():
    here = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(here, "..", ".."))
    return os.path.join(
        repo_root, "build", "apps", "espressolab_cli", "espressolab_cli"
    )


class Session:
    """A `tui` process attached to one end of a PTY."""

    def __init__(self, binary, size=None):
        """`size`, if given, is a (rows, cols) pair applied to the PTY
        before the process starts, so the app's very first frame already
        sees the constrained terminal (no SIGWINCH round-trip needed)."""
        self.master, slave = pty.openpty()
        if size is not None:
            rows, cols = size
            fcntl.ioctl(
                slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0)
            )
        self.proc = subprocess.Popen(
            [binary, "tui"], stdin=slave, stdout=slave, stderr=slave, close_fds=True
        )
        os.close(slave)

    def read(self, timeout=0.5):
        buf = b""
        while True:
            ready, _, _ = select.select([self.master], [], [], timeout)
            if not ready:
                break
            try:
                chunk = os.read(self.master, 65536)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            timeout = 0.05  # drain what's already buffered without a long final wait
        return buf

    def send(self, data):
        os.write(self.master, data)

    def read_until(self, needle, timeout=2.0):
        """Poll for `needle` (plain-text, escape codes stripped) to appear.

        Used before the first keystroke of each check: FTXUI only finishes
        entering raw mode and paints its first frame a short, unbounded time
        after the process starts, and a keystroke sent before that (Ctrl-C
        especially) can race the terminal's own signal generation instead of
        reaching the app as input. Waiting for real output is more reliable
        than a fixed sleep.
        """
        deadline = time.monotonic() + timeout
        buf = b""
        while time.monotonic() < deadline:
            buf += self.read(0.1)
            if needle in plain(buf.decode(errors="replace")):
                return buf
        return buf

    def wait(self, timeout=5.0):
        try:
            self.proc.wait(timeout=timeout)
            return self.proc.returncode
        except subprocess.TimeoutExpired:
            return None

    def kill(self):
        try:
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait(timeout=2.0)
        except Exception:
            pass
        try:
            os.close(self.master)
        except OSError:
            pass


def plain(text):
    return ESCAPE_RE.sub("", text)


def check(name, condition, detail=""):
    status = "PASS" if condition else "FAIL"
    print(f"{status}: {name}" + (f" -- {detail}" if detail and not condition else ""))
    return condition


def run(binary):
    results = []

    # -- non-interactive rejection: no PTY at all, just a redirected pipe.
    with open(os.devnull, "rb") as devnull_in:
        completed = subprocess.run(
            [binary, "tui"], stdin=devnull_in, capture_output=True, timeout=10
        )
    results.append(
        check(
            "non-TTY invocation fails fast with a stable exit code",
            completed.returncode == 3,
            f"exit code {completed.returncode}, stderr={completed.stderr!r}",
        )
    )
    results.append(
        check(
            "non-TTY invocation reports NONINTERACTIVE_TERMINAL",
            b"NONINTERACTIVE_TERMINAL" in completed.stderr,
            f"stderr={completed.stderr!r}",
        )
    )

    # -- launch on a real PTY and render the home screen. Wait for real output
    # rather than a fixed sleep: sending input before the app has finished
    # entering raw mode can race the terminal's own signal generation (see
    # the Ctrl-C check below).
    session = Session(binary)
    screen = plain(session.read_until("ESPRESSOLAB").decode(errors="replace"))
    results.append(
        check("home screen renders on launch", "ESPRESSOLAB" in screen, screen[:200])
    )

    # -- SIGWINCH must not crash the app; it should keep rendering afterward.
    session.proc.send_signal(signal.SIGWINCH)
    time.sleep(0.2)
    still_alive = session.proc.poll() is None
    results.append(check("process survives SIGWINCH (resize)", still_alive))
    after_resize = plain(session.read(0.3).decode(errors="replace"))
    results.append(
        check(
            "still renders after resize",
            still_alive and ("ESPRESSOLAB" in after_resize or after_resize == ""),
        )
    )

    # -- run a guided command that actually has form fields (`simulate`, the
    # first menu entry): open it, Tab past all 6 fields to the trailing Run
    # action without editing any of them (so it runs on its file defaults),
    # and confirm Enter on Run submits the job rather than just re-entering
    # edit mode on the last field. This is the path every other check here
    # skips by using a zero-field command, and it's the one that regressed.
    session.send(b"\r")  # open `simulate` (menu_index_ 0 already)
    time.sleep(0.3)
    form_screen = plain(session.read(0.3).decode(errors="replace"))
    results.append(
        check(
            "`simulate` form screen renders its fields",
            "Configure simulate" in form_screen,
            form_screen[-300:],
        )
    )
    for _ in range(6):  # recipe, coefficients, bean, dt, sample interval, out -> Run
        session.send(b"\t")
        time.sleep(0.05)
    on_run_screen = plain(session.read(0.3).decode(errors="replace"))
    results.append(
        check(
            "Tab reaches the trailing Run action",
            "> [ Run ]" in on_run_screen,
            on_run_screen[-300:],
        )
    )
    session.send(b"\r")  # Enter on Run must submit the job, not start editing
    time.sleep(0.5)
    simulate_result = plain(session.read(1.0).decode(errors="replace"))
    results.append(
        check(
            "Enter on Run actually runs `simulate` and shows its result",
            "result hash" in simulate_result and "beverage mass" in simulate_result,
            simulate_result[-400:],
        )
    )
    session.send(b"\x1b")  # back to the menu for the next check
    time.sleep(0.2)
    session.read(0.2)

    # -- run a representative guided command (`version`: no fields, instant).
    for _ in range(
        10
    ):  # simulate, sweep, calibrate, synthesize, bench, cfd, cfd3d, grind, params, fit-params -> version
        session.send(b"\x1b[B")
        time.sleep(0.05)
    session.send(b"\r")
    time.sleep(0.5)
    result_screen = plain(session.read(0.8).decode(errors="replace"))
    results.append(
        check(
            "`version` screen shows the native solver/schema line",
            "recipe-schema=" in result_screen,
            result_screen[-300:],
        )
    )

    # -- quitting restores the terminal and exits 0.
    session.send(b"q")
    time.sleep(0.3)
    tail = session.read(0.5)
    exit_code = session.wait(timeout=5.0)
    if exit_code is None:
        session.kill()
    results.append(
        check("`q` exits with status 0", exit_code == 0, f"exit code {exit_code}")
    )
    results.append(
        check(
            "leaves the alternate screen and shows the cursor on exit",
            b"\x1b[?1049l" in tail and b"\x1b[?25h" in tail,
            repr(tail[-120:]),
        )
    )

    # -- Ctrl-C from the menu exits cleanly (a fresh session: the prior one is gone).
    session2 = Session(binary)
    session2.read_until("ESPRESSOLAB")
    session2.send(b"\x03")  # ETX / Ctrl-C
    time.sleep(0.3)
    tail2 = session2.read(0.5)
    exit_code2 = session2.wait(timeout=5.0)
    if exit_code2 is None:
        session2.kill()
    results.append(
        check(
            "Ctrl-C from the menu exits with status 0",
            exit_code2 == 0,
            f"exit code {exit_code2}",
        )
    )
    results.append(
        check(
            "Ctrl-C exit also restores the terminal",
            b"\x1b[?1049l" in tail2,
            repr(tail2[-120:]),
        )
    )

    # -- a form taller than the terminal must scroll to reveal every field
    # and the trailing Run action (regression: `cfd3d` has 11 fields, and on
    # a short terminal the body used to render past the bottom of the screen
    # with no way to scroll down to it at all). Force a short PTY -- 15 rows
    # -- so the form cannot possibly fit without scrolling.
    session3 = Session(binary, size=(15, 80))
    session3.read_until("ESPRESSOLAB")
    for _ in range(6):  # simulate, sweep, calibrate, synthesize, bench, cfd -> cfd3d
        session3.send(b"\x1b[B")
        time.sleep(0.05)
    session3.send(b"\r")  # open cfd3d
    time.sleep(0.3)
    cfd3d_form = plain(session3.read(0.3).decode(errors="replace"))
    results.append(
        check(
            "`cfd3d` form screen renders on a short terminal",
            "Configure cfd3d" in cfd3d_form,
            cfd3d_form[-300:],
        )
    )
    reached_run = False
    last_screen = cfd3d_form
    for _ in range(15):  # cfd3d has 11 fields; this comfortably overshoots
        session3.send(b"\x1b[B")
        time.sleep(0.05)
        chunk = plain(session3.read(0.2).decode(errors="replace"))
        if chunk:
            last_screen = chunk
        if "> [ Run ]" in last_screen:
            reached_run = True
            break
    results.append(
        check(
            "short terminal scrolls down to the focused Run action",
            reached_run,
            last_screen[-300:],
        )
    )
    session3.send(b"\x1b")  # back to the menu without running the solve
    time.sleep(0.2)
    session3.kill()

    return results


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else default_binary()
    if not os.path.isfile(binary) or not os.access(binary, os.X_OK):
        print(f"SKIP: prerequisite missing -- no executable at {binary}")
        print("      build it first, e.g. `./scripts/build.sh`")
        sys.exit(1)
    if not sys.stdout.isatty() and os.environ.get("ESPRESSOLAB_PTY_SMOKE_FORCE") != "1":
        # openpty() itself does not need a controlling terminal, but a CI
        # runner with no PTY support at all should say so plainly rather than
        # fail confusingly deep inside the checks below.
        try:
            pty.openpty()
        except OSError as error:
            print(f"SKIP: prerequisite missing -- cannot open a PTY here ({error})")
            sys.exit(1)

    print(f"binary: {binary}\n")
    results = run(binary)
    passed = sum(1 for r in results if r)
    print(f"\n{passed}/{len(results)} checks passed")
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
