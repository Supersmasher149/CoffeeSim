// Placeholder for the measured-shot calibration view. Calibration is a separate,
// explicit workflow (11.3), and the dashboard side of it is the second thing cut
// when the schedule slips (15.2) — the CLI and the coefficient files keep working
// without it.
export function CalibrationNotice({ coefficientId, coefficientVersion }: {
  coefficientId: string;
  coefficientVersion: string;
}) {
  return (
    <details className="drawer">
      <summary>Calibration — coefficient set {coefficientId} v{coefficientVersion}</summary>
      <div className="body">
        <p className="note">
          No measured shot has been fitted to this coefficient set yet, so every curve above is an
          uncalibrated model output rather than a prediction. The calibration workflow lives in the
          CLI and in <code>assets/measured_shots/</code>: record a baseline shot, minimise the
          weighted error of section 11.4 across several shots, hold one shot back as a validation
          case, and commit the fitted coefficient file with its dataset reference and limitations.
        </p>
      </div>
    </details>
  );
}
