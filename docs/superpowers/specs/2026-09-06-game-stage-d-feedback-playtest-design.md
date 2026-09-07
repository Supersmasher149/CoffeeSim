# Game Stage D: Feedback and Formative Playtest

## Status

Approved design. Stage D extends the existing React Game surface without
changing the native shot, API, JSON, or schema contracts. The session remains
limited to two shots. The playtest is documented manually in the repository;
the application does not collect telemetry or tester responses.

## Goals

- Replace the current generic Game labels with bounded, authored feedback based
  on the recorded grind-only sweep.
- Ask the player to predict the next grind direction before exposing an
  optional hint.
- Show one plain-language observed-change sentence after the second shot,
  using only differences supported by native result fields.
- Add automated coverage for feedback boundaries, hint gating, and the
  two-shot comparison flow.
- Add a no-coaching manual protocol and results template for 3-5 people who are
  unfamiliar with EspressoLab.

## Non-goals

- No third attempt, rewards, penalties, progression, or session persistence.
- No new native calculation, server route, result field, schema field, or
  browser-side replacement for authoritative metrics.
- No taste-validation claim. Game feedback remains a heuristic over native
  engineering outputs.
- No in-app feedback form, analytics, or user-identifying data collection.
- No rerun of the Stage A native sweep as part of Stage D. The previously
  recorded values below are the frozen classifier basis.

## Frozen Evidence

The existing Stage A record used solver `solver-0.4.0`, coefficients `default`
version `v1.0.0`, a fixed baseline recipe, and the five Game scalar settings.
All runs had zero warnings. The values are:

| Game setting | Diameter (um) | Termination | Time (s) | Beverage (g) | TDS (%) | Yield (%) | Authored outcome |
| --- | ---: | --- | ---: | ---: | ---: | ---: | --- |
| Coarse | 450 | target mass | 19.88 | 36.04 | 6.49 | 12.99 | BEAN TEA |
| Medium-coarse | 400 | target mass | 23.57 | 36.02 | 7.72 | 15.45 | BEAN TEA |
| Baseline | 350 | target mass | 29.03 | 36.01 | 9.09 | 18.18 | TARGET MATCHED |
| Medium-fine | 300 | target mass | 37.66 | 36.01 | 10.51 | 21.02 | intermediate |
| Fine | 250 | time limit | 45.00 | 25.88 | 12.92 | 18.58 | BARELY A SIP |

These are authored engineering bands for this Game baseline, not general
espresso rules. If the baseline inputs or native coefficients change, the
classifier version and evidence must be revisited rather than silently
reusing these bands.

## Feedback Model

`web/src/features/game/gameRules.ts` remains the pure feedback boundary. The
classifier version changes to `game-result-classifier-2`. Its outcome type is:

- `bean_tea`: target mass reached in at most 24 seconds, with TDS at most 8%
  and extraction yield at most 16%.
- `barely_a_sip`: target mass was not reached or beverage mass is below 34 g.
- `target_matched`: beverage mass is 34-38 g, elapsed time is 26-36 seconds,
  TDS is 8-10%, and extraction yield is 16-20%.
- `intermediate`: a valid native result outside the other authored bands.
- `simulation_note`: a valid result has a hard solver warning or a diagnostic
  `clamp_count` greater than zero. It is never scored as a player outcome.

Safety precedence is evaluated before the normal bands. Invalid JSON, missing
fields, non-finite values, unsupported terminations, numerical failures, and
invalid states continue to be rejected by `validateGameResult()` and never
reach the classifier. Informational and soft warnings remain visible but do
not override scoring; hard warnings use `simulation_note`.

The user-facing copy is fixed and uppercase for the three named outcomes:

| Outcome | Title | Default recommendation |
| --- | --- | --- |
| `bean_tea` | BEAN TEA | Try one step finer. |
| `barely_a_sip` | BARELY A SIP | Try one step coarser. |
| `target_matched` | TARGET MATCHED | Hold this setting. |
| `intermediate` | IN BETWEEN | The native result sits between the tested outcomes. |
| `simulation_note` | SIMULATION NOTE | This run used a solver guardrail and is not scored. |

The selected `GameGrindIndex` is passed to the classifier only for hint
eligibility. Classification remains based on the native result metrics. The
frozen neighbor evidence allows these directional hints:

- 450 um: finer, moving toward the tested 400 um result.
- 400 um: finer, with 350 um as the tested target match.
- 350 um: stay, because it is the tested target match.
- 300 um: coarser, with 350 um as the tested target match.
- 250 um: coarser, moving toward the tested 300 um result.

If the actual result does not match the expected frozen setting profile, or if
the result is `simulation_note`, no directional hint is exposed. The hint
must not claim that the native solver predicts taste.

## Interaction and State

`ComparePhase` owns transient prediction and hint visibility. No prediction or
hint fields are added to `GameAttempt` because they are not native provenance
and are not needed to preserve result immutability.

After shot 1 completes:

1. Render the native metrics and the outcome immediately.
2. Keep `Adjust grind for shot 2` available without requiring the player to
   answer a question.
3. Show the question, "Before a hint, which way would you adjust?"
4. Provide explicit `Finer`, `Stay`, and `Coarser` buttons with `aria-pressed`.
5. Keep the hint hidden until a prediction is selected. Then reveal the
   validated hint inline with no additional explanation or reading step.

Skipping the prediction means skipping the hint; it does not block the next
shot. A safety-only result has no directional hint. The existing retry,
replay, mute, reduced-motion, and two-shot behavior remains unchanged.

After shot 2 completes, render an `Observed change` note instead of the
prediction block. The note is shown only when the first accepted attempt is
available and compares the two native results.

## Observed Change

The pure observed-change helper uses only `elapsed_time_s` and `tds_percent`.
These display-only thresholds make the statement resistant to insignificant
rounding noise:

- elapsed time is meaningful when its absolute difference is at least 1.0 s;
- measured strength is meaningful when its absolute TDS difference is at least
  0.25 percentage points.

When both differences are meaningful, the sentence states both directions,
for example, "The second setting filled the cup 5.5 s faster and measured
1.37 percentage points lower TDS." When only one is meaningful, the other
quantity is described as nearly unchanged. When neither is meaningful, the
sentence says the runs were close on fill time and measured strength. The
helper never infers a taste or causal claim from these differences.

## Component Changes

The implementation will stay within the existing Game boundary:

- `gameRules.ts`: outcome bands, version, copy, neighbor hint eligibility,
  and observed-change helper.
- `gameRules.test.ts`: outcome boundaries, safety precedence, hint mapping,
  and observed-change cases.
- `ComparePhase.tsx`: prediction buttons, hint reveal, and observed-change
  note with accessible names and live status where appropriate.
- `GameWorkspace.tsx`: pass the selected setting to classification and the
  prior accepted attempt to the second Compare view.
- `GameWorkspace.test.tsx`: end-to-end component coverage for prediction,
  hint gating, skip behavior, and second-shot observed feedback.
- `styles.css`: compact feedback, prediction, hint, and observed-change
  styles that stack on mobile and retain the existing Game visual language.
- `web/e2e/game.spec.ts`: native-server assertions for named outcomes,
  prediction-before-hint ordering, and the second-shot observation.
- `web/src/a11y.test.tsx`: a completed feedback state with no serious or
  critical accessibility violations.

No native or REST files are in scope.

## Manual Playtest Artifact

Add `docs/game-playtest-stage-d.md` as a repository-based protocol and blank
results sheet. It will contain:

- the purpose, fixed two-shot scope, environment/build fields, and no-coaching
  facilitator script;
- instructions to let each tester start from the Game objective, make their
  own first grind choice, predict finer/stay/coarser before any hint, complete
  the second shot, and decide whether to use `Play again`;
- one row per tester for tester code, completion time, completion, predicted
  direction, hint use, explanation of one observed change, voluntary replay,
  and one friction point;
- a round summary for comprehension, completion, hint use, replay, and the
  largest feedback or usability problem;
- a second-round section for the same short protocol after fixing exactly one
  largest problem.

The facilitator must not teach the grind relationship, explain the labels, or
turn the session into a taste-validation exercise. The suggested directional
decision rule is that most testers complete and explain the relationship and
at least two choose `Play again`. The sample is formative evidence, not a
statistical claim. The implementation cannot claim the playtest gate is met
until people unfamiliar with the simulator have completed the manual protocol.

## Verification

Before calling the engineering portion complete, run the focused Game tests,
web typecheck, coverage, production build, and the existing native-server
desktop/mobile Playwright flow. Verification must cover:

- all five frozen settings and exact band boundaries;
- guard-clamped and hard-warning overrides;
- prediction buttons, hidden-before-selection hints, and skip behavior;
- observed-change wording for faster/slower, stronger/lighter, and close runs;
- accessibility of completed feedback;
- unchanged native request shape and two-shot result immutability.

The manual 3-5-person protocol is a separate acceptance gate. After its first
round, one largest feedback or usability problem is fixed and the short test
is repeated. No additional feature is added unless it directly resolves that
observed problem.
