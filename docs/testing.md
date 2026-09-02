# Testing strategy

```bash
./scripts/test.sh                  # build Release and run the Catch2 binary
./build/tests/espressolab_tests "[flow]"   # one tag
./build/tests/espressolab_tests --list-tests
```

Tags: `[unit]` `[units]` `[profile]` `[water]` `[permeability]` `[flow]` `[heat]`
`[extraction]` `[artifacts]` `[integration]` `[invariants]` `[convergence]`
`[sweep]` `[calibration]` `[recovery]` `[property]` `[performance]` `[regions]`
`[axial]` `[cfd]` `[cfd3d]` `[verification]` `[references]` `[progress]`
`[cancellation]` `[tui]` `[cli_workflows]` `[grind]` `[grind_sim]` `[flavor]`.

```bash
python3 tests/pty/tui_smoke.py    # separate POSIX PTY smoke matrix for the TUI
ctest --test-dir build -L server  # black-box REST/CLI smoke scripts, registered but not in test.sh
```

`./scripts/test.sh` does not run the PTY script, the `server`-labeled ctest
smoke scripts, dashboard checks, demo, or warnings-as-errors build. The web
checks are separate: `npm --prefix web run typecheck`,
`npm --prefix web run test:coverage`, and `npm --prefix web run build` cover
static, runtime, and production-build behavior. `npm --prefix web run test:e2e`
runs the Chromium desktop/mobile browser suite against a built native server;
`npm --prefix web run test:e2e:all` adds the nightly Firefox and WebKit matrix.

## What each layer is for

**Unit tests** pin the pieces that have a right answer: conversions, profile
interpolation and its boundaries, water-table knots, the monotone response of
permeability to particle size, zero flow at zero pressure, no heat transfer at
zero temperature difference, no extraction from a dry puck, and stable error
codes and paths from the loaders.

**Integration tests** run whole shots and check behaviour rather than numbers:
the baseline stops on its mass target with no hard warnings, a coarse puck runs
fast and weak, a choked puck stalls with a warning instead of a numerical
failure, pre-infusion delays first drops relative to immediate pressure, and the
  same inputs reproduce the same result hash on the same build.

**Property and invariant tests** generate 200 valid recipes from a fixed seed
and require that none produce NaN or infinity, that masses stay nonnegative,
that beverage mass and extraction yield never go backwards, and that both mass
balances close to within 1e-9 kg. A second sweep generates 60 random one-to-
eight region partitions and holds the same bounds over each one, because a
partition is an input the hand-written fixtures cannot cover exhaustively.

**Parallel-region tests** cover fidelity level 2 specifically. A uniform single
region has to reproduce the Level 1 aggregate; an asymmetric two-region puck has
to give its higher-permeability region the larger share of integrated flow; the
water and solids residuals have to close across an eight-way unequal split,
where a per-region bookkeeping slip cancels in the aggregate only by luck; and
the `regions` array has to survive into the serialized summary and result with
the configured partition echoed back in recipe order. That last one runs a real
shot and parses the JSON, because the hash test beside it builds its region
summaries by hand and would still pass if the serializer stopped emitting
them.

**Axial cell tests** cover fidelity level 3. One cell has to reproduce the
Level 2 shot; doubling the cell count has to move shot time and yield by a
shrinking amount, because a discretization that does not settle is not resolving
anything; a run stopped mid pre-infusion has to show a wetting front, meaning no
cell drier than the one above it and a real edge somewhere in the column; pore
concentration has to rise and local yield has to fall with depth, which is the
mechanism the lumped puck could not express; the balances have to close at 1, 3,
8 and 32 cells; and the per-cell summary has to reach the serialized artifacts
in screen-to-basket order.

**Particle size distribution tests** (`[grind]`) guard the second grind
spelling against the first. The load-bearing pair is the two "scalar recipes
keep their pre-PSD hashes" cases: a recipe written with
`particle_diameter_um`/`particle_spread_factor` has to produce the same recipe
hash and the same result hash it produced before the distribution path
existed, which is what makes `puck.grind` an addition rather than a silent
contract change. Beyond that: a single-bin distribution has to reproduce the
scalar shot exactly, d32 has to match the hand-computed harmonic form and get
dragged down by a small mass of fines, permeability at d32 has to equal the
scalar call, a size-resolved shot has to close its mass balances, and a
distribution has to extract *less* than its own d32 would, because fines
deplete first. The serialization cases pin the fixed point (load → dump →
load → dump is exact), the mutual exclusion of the two spellings
(`CONFLICTING_FIELD`), and the sweep behaviour: sweeping grind size rescales
every bin, while sweeping the spread of a distribution is refused rather than
guessed.

**Sensory overlay tests** (`[flavor]`) cover bean documents and the flavour
layer. The load-bearing one is in `[artifacts]`: recipe and coefficient hashes
are pinned, while physical outputs are checked to tight tolerances and repeated
result hashes are checked for determinism. A result hash is not pinned as a
literal because its sample-series formatting includes platform-libm-sensitive
last-ulp values. The integration tests assert the rest bit-for-bit -- attaching
a bean must leave every sample, region and summary field equal -- and otherwise
make only relative claims (a light natural reads fruitier than a dark roast),
never absolute intensities, because no absolute intensity means anything until a
tasting panel has been run.

**Grinder tests** (`[grind_sim]`) cover the comminution model behind
`espressolab_cli grind`, which is outside the shot pipeline and owns no result
hash. Breakage has to conserve mass exactly; the emitted distribution has to be
one a recipe can actually carry; a finer burr gap has to give a finer grind and
more passes a finer one still, with diminishing return; the distribution has to
be bimodal rather than merely broad, since the fines peak is the whole point;
and the same spec always has to give the same distribution. A round-trip case
pastes a generated distribution into a recipe and runs it, which is the only
place the grinder and the solver meet.

**Parallel sweep tests** (`[sweep]`, plus `[unit]` ring-buffer tests) exist
because `espressolab_cli sweep --workers` added a second execution path, and
two paths that disagree are worse than one slow one. The load-bearing case
requires the parallel batch runner to produce the same runs, in the same
order, with the same per-run result hashes as `ExperimentRunner::run` for the
same spec — concurrency may change arrival order at the consumer, never a
run's content or its final position. A ring smaller than the worker count
still has to deliver every run in order (the back-pressure path), cancellation
has to leave an unbroken index prefix rather than a hole, an out-of-range
corner has to be recorded rather than thrown, and an invalid spec has to be
rejected before any worker thread starts. The `BoundedRingBuffer` underneath
is tested separately: zero capacity is rejected, capacity actually bounds what
is pending, multiple producers and one consumer see every value exactly once,
and `pop` returns false only once the buffer is closed *and* drained.

**CFD verification tests** are a different kind of test from everything else
here, and the distinction matters. They do not ask whether the solver describes
espresso; they ask whether it solves the equations it claims to. The discrete
divergence of the total velocity field has to be ~1e-8 1/s against Darcy
velocities four orders of magnitude larger, or the elliptic solve is not
converged and nothing downstream means anything. Water and solute balances have
to close at ~1e-17 kg, which the face-based limiter makes structural rather than
approximate. A uniform bed has to produce an axisymmetric solution, since radial
structure there would be an artefact of the mesh metrics. An isothermal column
is a method-of-exact-solutions check: uniform mobility reduces the pressure
equation to `d2p/dz2 = 0`, so the field must be linear in depth, and the residual
5e-4 departure is identified as the saturation profile by the fact that it
shrinks under refinement. The Darcy velocity is compared against the analytic
value from the same coefficients. Refinement has to converge, and repeated runs
have to reproduce identical fields.

**Convergence tests** run the baseline at 0.02, 0.01 and 0.005 s and require the
change to shrink as the step halves. This is the test that would catch a
scientifically wrong integration scheme hiding behind a plausible curve.

**Calibration recovery tests** are how a fitting routine is checked without real
data. Known coefficients are hidden inside synthetic shots, the fit is started
from a deliberately wrong point, and the test requires it to find the truth again
within 2%. Companion tests check that the fit is deterministic, that held-out
shots never steer it, that coefficients outside the parameter list do not move,
and that synthetic provenance survives into the report and the fitted file. This
validates the machinery; it says nothing about whether the model matches real
espresso.

**Leave-one-out tests** verify that each shot is held out exactly once, aggregate
metrics remain deterministic, acceptance thresholds reject poor held-out fits,
and synthetic, mixed-machine, or incomplete datasets are refused before they
can be described as real-world validation. The tests use model-generated inputs
as fixtures and explicitly avoid treating them as real calibration evidence.

**Measured-shot comparison tests** cover strict stored-asset loading, selector
resolution, response metadata, optional measurement flags, and residuals. The
result-based comparison API cannot invoke the simulator, so the server runs once
and passes that result to the same native scoring operation used by calibration.
Synthetic fixtures test plumbing only and remain labelled synthetic in every
response. `tests/server/measured_shots_smoke.py` covers the real HTTP routes.

**Cancellation tests** call `Simulator::run`, `CfdSolver::run`, `Cfd3dSolver::run`,
`calibration::fit`, and `calibration::evaluate_shot_loss` with a callback that
always reports "cancelled" and require `ExecutionCancelled` rather than a
completed result -- the checkpoints the TUI's cancel button and Ctrl-C
handling rely on. A companion test in `test_cli_workflows.cpp` requires that a
cancelled `run_simulate` never reaches the artifact writer: the output
directory must not exist afterward. Sweep cancellation and partial-run export
are covered separately, in `test_sweeps.cpp`, since `ExperimentRunner` predates
this contract.

**REST integration tests** (Audit P1, issue #17) close the gap the other
layers above cannot: `apps/espressolab_server/main.cpp` is not linked into
`espressolab_tests`, so nothing above exercises the actual HTTP request
translation, error-response mapping, background-job polling, or the
in-memory stores' retention behavior. `tests/server/rest_integration_smoke.py`
starts the real built server on an isolated local port and drives the
documented contract end to end: health; a valid shot and three ways to
reject one (malformed JSON, a missing field, an out-of-range recipe);
retrieval by id and a 404 for an unknown one; CSV export for a shot, a
completed sweep, a still-running sweep (409), and an unknown id (404); a
sweep's full queue-poll-list lifecycle; cancelling a large sweep
immediately after queuing it, which leaves it "cancelled" with fewer
completed runs than its total; and `RunStore`'s 128-shot FIFO retention,
exercised by posting 129 distinct shots (varying dose_g -- run_id is
deterministic over the recipe, so identical requests would just overwrite
one entry rather than growing the store) and confirming the first is
evicted. It joins the other `tests/server/*.py` (`ctest --test-dir build -L
server`) and `tests/cli/*.py` (`ctest --test-dir build -L cli`) black-box
scripts as a registered, not-in-`test.sh` layer: each starts a real built
binary as a subprocess, since neither `apps/espressolab_server/main.cpp` nor
`apps/espressolab_cli/main.cpp` is linked into `espressolab_tests`.
`tests/cli/malformed_flags_check.py` covers `main.cpp`'s numeric flag
parsing, which `test_cli_workflows.cpp` cannot reach.

**TUI tests** (`[tui]`, `[cli_workflows]`) are pure and terminal-free: they
call `tui_forms.cpp`'s navigation, field defaults, and validation directly
(never through FTXUI), and `workflows.cpp`'s shared services directly (never
through argv parsing). One test checks that `run_simulate`'s result hash
matches calling `Simulator::run` directly with the same recipe and
coefficients -- the equivalence the legacy CLI and the TUI both depend on now
that they call the same function. What these tests cannot cover -- FTXUI
rendering, raw-mode/alternate-screen lifecycle, resize, and real Ctrl-C
delivery through a terminal's line discipline -- is the separate PTY smoke
matrix's job; see `tests/pty/tui_smoke.py`.

## Frontend runtime tests

The dashboard (`web/`) has its own layered suite, separate from the Catch2
binary above and run by the `web` and `web-e2e` CI jobs rather than
`./scripts/test.sh`:

```bash
npm --prefix web run test              # Vitest: unit + component + accessibility
npm --prefix web run test:watch
npm --prefix web run test:coverage     # same, with coverage thresholds enforced
npm --prefix web run test:e2e          # Playwright: chromium + chromium-mobile
npm --prefix web run test:e2e:all      # + firefox + webkit + a mobile webkit viewport
```

**Unit tests** (`web/src/state/workspace.test.ts`, `web/src/api/client.test.ts`)
cover the two modules everything else depends on: every `localValidation`
boundary (exact, just inside, just outside, NaN/Infinity, scalar vs. PSD,
null target mass, unordered/duplicate/empty profiles), `preInfusionEnd`'s
immediate-peak and repeated-peak cases, and the API client's HTTP methods,
query/path encoding (including reserved characters in measured-shot and
coefficient identifiers), `AbortSignal` forwarding, and every branch of
`ApiFailure` parsing (structured error, missing `details.issues`, non-JSON
body, network rejection). These two files carry their own, higher coverage
thresholds in `web/vitest.config.ts`.

**Component tests** (React Testing Library, one file per component under
`web/src/features/`, `web/src/app/`, plus `web/src/App.test.tsx`) run against a mock server
(`msw`, `web/src/test/fixtures/server.ts`) so they exercise the same
`fetch()` path production code takes rather than a mocked API client. They
cover loading/empty/error states, request races (measured-shot comparison's
abort-on-reselect and disabled-while-loading guards; sweep polling,
cancellation, and terminal states with real `setTimeout`-driven polling
rather than fake timers, which fought `userEvent` and `msw` in practice),
immutable-patch edits, keyboard interaction (arrow-key profile point
movement, snapping, neighbour fencing), and the two production bugs this
suite's construction found and fixed: `App.tsx` was handing
`ReferenceShotsPanel` the live draft recipe instead of the recipe that
produced the active result (same class of bug the `activeRecipe` contract
already guarded against for `PuckView`/`ChartStack` -- see the `Audit P7,
issue #22` comments), and several controls (`ControlRail`'s number fields,
`ComparisonTray`'s remove buttons, `SweepPanel`'s axis fields and progress
bar, `HeatMap`'s cells) had no accessible name or label association at all.
The workbench-shell tests also pin semantic tab selection and arrow/Home/End
keyboard navigation, while App tests cross tabs before asserting workflow-local
content so hidden panels cannot accidentally leak into the accessibility tree.
`web/src/test/setup.ts` centralises the jsdom gaps every one of these files
would otherwise hit individually: `ResizeObserver`, `matchMedia`,
`requestAnimationFrame`, Blob URLs, and the SVG measurement/coordinate APIs
(`getBBox`, `createSVGPoint`, `getScreenCTM`) Recharts and `ProfileCanvas`
both call.

**Accessibility tests** (`web/src/a11y.test.tsx`) run `axe-core` (via
`vitest-axe`) against five stable states -- the empty app, a completed
simulation, a completed measured-shot comparison, a completed two-axis sweep,
and an open diagnostics drawer/profile editor -- and fail only on
serious/critical violations (`web/src/test/a11y.ts`); moderate/minor findings
are real but noisier, and gating on them would fail this check for reasons
unrelated to an actual regression.

**Playwright tests** (`web/e2e/`) are the only layer that runs a real browser
against the real built `espressolab_server` binary, per
`web/playwright.config.ts`'s two-stage `webServer`: the native server on an
isolated port, then Vite with its dev-proxy target pointed at that port via
`ESPRESSOLAB_SERVER_URL` (`web/vite.config.ts`). They cover what jsdom cannot:
real SVG pointer geometry and `getScreenCTM` (dragging a profile point),
keyboard navigation through actual focus order, file downloads (CSV/JSON,
inspected after `page.waitForEvent("download")`), deterministic result
hashes across two real solver runs, and that comparing a measured shot or
pinning a run behaves the same way against the real server as the mocked
component tests already proved. Desktop and mobile Chromium run in CI on
every push and PR; the nightly schedule adds Firefox, WebKit and a mobile
WebKit viewport. CI retries a failure once and still records it as flaky
rather than a clean pass, and uploads the HTML report plus every failing
test's trace/screenshot/video.

There is no separate visual-regression layer yet -- Playwright's
`screenshot: "only-on-failure"` is diagnostic only, not a maintained set of
golden images.

## On golden fixtures

There is deliberately no exact-snapshot golden result. Snapshots make every
beneficial model change look like a regression, and they pass just as happily
when the physics is subtly wrong. The invariants, the mass balances and the
convergence test are what actually guard the model; the result **hash** is used
for reproducibility, not for correctness.

## The acceptance test

`./scripts/demo.sh` is the local acceptance workflow: from a clean clone, run
the baseline recipe, complete a grind-size sweep, export JSON and CSV, and rerun
the same inputs to the same result hash. GitHub Actions macOS, Linux, and
dashboard, and full nightly browser-matrix jobs passed for current `origin/main`
at commit `347177c` in [run 33508004359](https://github.com/Supersmasher149/CoffeeSim/actions/runs/33508004359).
Future changes need their own hosted run; do not extend this evidence to an
unrelated branch. See
[current-state-and-gaps.md](current-state-and-gaps.md) for the evidence status.
