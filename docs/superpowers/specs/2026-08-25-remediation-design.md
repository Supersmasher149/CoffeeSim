# EspressoLab Remediation Design

## Scope

This remediation pass addresses all review findings: local-server safety and
input handling, simulation and calibration correctness, CLI guardrails,
dashboard accessibility and markup, and README figure use. It deliberately
does not introduce accounts, persistence, a worker pool, or new dashboard
features.

## Server Safety And API Behavior

- Sweep identifiers will derive from a route-safe slug of the submitted name,
  plus the existing monotonic serial. The display name remains unchanged in the
  completed sweep result.
- The in-memory shot store will retain a fixed FIFO number of the most recent
  shots. Older IDs will return the documented 404 result.
- Sweep worker handles will be joined and discarded once their jobs finish,
  before later sweep creation. Active workers retain their current behavior;
  this change prevents completed thread handles from accumulating over a long
  session.
- The server will not advertise cross-origin access. The dashboard already uses
  Vite's same-origin development proxy, so no CORS response headers are needed.
- Request handlers will require JSON objects before reading named fields, and
  malformed object/array shapes will produce the documented structured 400
  response rather than reaching the generic 500 handler.
- Duplicate sweep parameter paths will be rejected before the background job
  starts.

## Artifact Parsing And Simulation

- Coefficient documents will require an object root, string `id` and `version`,
  an object `values`, and every serialized coefficient as a finite number.
  Missing or wrongly typed values will report a structured loader error; no
  malformed value may silently retain a default.
- The simulator will emit values at exact requested sample timestamps by
  interpolating each state transition. The termination timestamp remains the
  final sample even when it is not an interval boundary.
- Average flow will be calculated as flow integrated over elapsed time, rather
  than an unweighted average of output samples.
- Result hashes will include saturation because it is a serialized output field.

## Calibration And CLI

- Calibration specs will validate every named tunable parameter, parameter
  bounds, duplicate names, finite loss weights and solver controls, positive
  iteration limits, and nonnegative finite tolerance before optimization.
- Nelder-Mead will use outside contraction when reflection is better than the
  worst vertex, and inside contraction otherwise.
- `bench --repeats` must be positive. Zero and negative values return a CLI
  input error before allocating or indexing benchmark samples.

## Dashboard And Documentation

- Profile point controls will expose keyboard movement with bounded, snapped
  adjustments and screen-reader labels/instructions, while retaining pointer
  drag and numeric editing.
- CSV downloads will be anchors styled as controls, not a button nested in an
  anchor.
- The generated light/dark architecture and sweep figures will be embedded in
  the README using GitHub-compatible `picture` markup. The existing generator
  remains their traceable source.

## Validation

- Add native regression tests for strict coefficient parsing, exact sampling,
  time-integrated average flow, saturation hashing, duplicate sweep axes, and
  calibration-spec validation and contraction behavior.
- Run the native suite, production dashboard build, CLI demo and deterministic
  result-hash check, plus a negative `bench --repeats 0` check.
