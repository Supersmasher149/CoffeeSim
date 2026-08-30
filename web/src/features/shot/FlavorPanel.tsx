import { SENSORY_AXES, SOLUTE_CLASSES } from "../../api/types";
import type { FlavorResult, SensoryAxis, SoluteClass } from "../../api/types";

// The flavour overlay's read-only view. Every number here is computed by the
// native solver and rendered as returned -- the browser derives no authoritative
// quantity (CLAUDE.md, "Data contracts").

const VERDICT_LABEL: Record<FlavorResult["verdict"], string> = {
  under_extracted_sour: "Bright for this bean's target",
  balanced: "Balanced for this bean's target",
  over_extracted_bitter: "Heavy for this bean's target",
};

const CLASS_LABEL: Record<SoluteClass, string> = {
  acids: "Acids",
  sugars: "Sugars",
  maillard: "Maillard",
  lipids: "Lipids",
  bitter: "Bitter",
  polyphenols: "Polyphenols",
};

function AxisBar({ axis, intensity, target }: { axis: SensoryAxis; intensity: number; target: number }) {
  const clamped = Math.max(0, Math.min(10, intensity));
  const deviation = intensity - target;
  return (
    <div className="flavor-axis">
      <div className="flavor-axis-name">{axis}</div>
      <div className="flavor-track">
        <div className="flavor-fill" style={{ width: `${clamped * 10}%` }} />
        {/* The roaster's declared target, so the bar is read against the coffee
            rather than against an absolute scale nobody has calibrated. */}
        <div
          className="flavor-target"
          style={{ left: `${Math.max(0, Math.min(10, target)) * 10}%` }}
          title={`target ${target.toFixed(1)}`}
        />
      </div>
      <div className="flavor-axis-value">
        {intensity.toFixed(1)}
        <span className="flavor-axis-dev">
          {deviation >= 0 ? "+" : ""}
          {deviation.toFixed(1)}
        </span>
      </div>
    </div>
  );
}

export function FlavorPanel({ flavor }: { flavor: FlavorResult }) {
  return (
    <div className="chart-card">
      <h3>
        Sensory estimate — {flavor.bean_id}
        <span className="flavor-heuristic"> heuristic, uncalibrated</span>
      </h3>

      <div className="flavor-headline">
        <div>
          <span className="flavor-score">{flavor.match_score.toFixed(0)}</span>
          <span className="unit">/ 100 vs target</span>
        </div>
        <div className="flavor-verdict">{VERDICT_LABEL[flavor.verdict]}</div>
        <div className="flavor-dominant">furthest off: {flavor.dominant_deviation_axis}</div>
      </div>

      <div className="flavor-axes">
        {SENSORY_AXES.map((axis) => (
          <AxisBar
            key={axis}
            axis={axis}
            intensity={flavor.axes[axis].intensity}
            target={flavor.axes[axis].target}
          />
        ))}
      </div>

      <div className="flavor-composition">
        {SOLUTE_CLASSES.map((klass) => (
          <span key={klass} className="flavor-chip">
            {CLASS_LABEL[klass]} <b>{flavor.composition_percent[klass].toFixed(1)}%</b>
          </span>
        ))}
      </div>

      {/* Stated here and not only in the diagnostics drawer: someone who never
          opens the drawer must still see what these numbers are worth. */}
      <p className="flavor-caveat">
        A heuristic overlay on the extraction the solver computed, from authored
        solute-class priors that have never been compared with a tasting panel. It
        changes no mass, TDS or yield. Scores are relative to this bean's declared
        target, not a measure of quality.
      </p>
    </div>
  );
}
