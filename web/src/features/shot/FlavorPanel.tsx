import { useEffect, useId, useState } from "react";

import { api } from "../../api/client";
import { SENSORY_AXES, SOLUTE_CLASSES } from "../../api/types";
import type {
  FlavorResult,
  SensoryAxis,
  SoluteClass,
  TastingAxisReading,
  TastingReading,
  TastingReadingCatalogue,
} from "../../api/types";
import { isTastingReading, levelOfIntensity, tastingBand } from "./tasting";

// The flavour overlay's read-only view. Every number here is computed by the
// native solver and rendered as returned -- the browser derives no authoritative
// quantity (CLAUDE.md, "Data contracts"). A tasting reading, when one is picked,
// is drawn beside it as returned by the server; it is a model's reading of a
// taster's notes, not a model output, and nothing here feeds it back.

const VERDICT_LABEL: Record<FlavorResult["verdict"], string> = {
  under_extracted_sour: "Bright for this bean's target",
  balanced: "Balanced for this bean's target",
  over_extracted_bitter: "Heavy for this bean's target",
};

const TASTED_VERDICT_LABEL: Record<FlavorResult["verdict"], string> = {
  under_extracted_sour: "Taster: sour / under-extracted",
  balanced: "Taster: balanced",
  over_extracted_bitter: "Taster: bitter / over-extracted",
};

const TASTED_STATUS_LABEL: Record<string, string> = {
  not_mentioned: "not mentioned",
  low_confidence: "unsure",
};

const CLASS_LABEL: Record<SoluteClass, string> = {
  acids: "Acids",
  sugars: "Sugars",
  maillard: "Maillard",
  lipids: "Lipids",
  bitter: "Bitter",
  polyphenols: "Polyphenols",
};

function tastedText(tasted: TastingAxisReading) {
  return tasted.level ?? TASTED_STATUS_LABEL[tasted.status] ?? tasted.status;
}

function AxisBar({
  axis,
  intensity,
  target,
  tasted,
}: {
  axis: SensoryAxis;
  intensity: number;
  target: number;
  tasted?: TastingAxisReading;
}) {
  const clamped = Math.max(0, Math.min(10, intensity));
  const deviation = intensity - target;
  const band = tasted?.level ? tastingBand(tasted.level) : undefined;
  return (
    <div className="flavor-axis">
      <div className="flavor-axis-name">{axis}</div>
      <div className="flavor-track">
        {band && (
          <div
            className="flavor-tasted-band"
            aria-hidden="true"
            style={{ left: `${band[0] * 10}%`, width: `${(band[1] - band[0]) * 10}%` }}
          />
        )}
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
      {tasted && (
        <div
          className={`flavor-axis-tasted${tasted.level ? "" : " muted"}`}
          title={`confidence ${(tasted.confidence * 100).toFixed(0)}%`}
        >
          {tastedText(tasted)}
        </div>
      )}
    </div>
  );
}

function useTastingReadings() {
  const [catalogue, setCatalogue] = useState<TastingReadingCatalogue>();
  const [error, setError] = useState<string>();
  useEffect(() => {
    const controller = new AbortController();
    api
      .tastingReadings(controller.signal)
      .then(setCatalogue)
      .catch((failure) => {
        if (failure instanceof DOMException && failure.name === "AbortError") return;
        setError(failure instanceof Error ? failure.message : String(failure));
      });
    return () => controller.abort();
  }, []);
  return { catalogue, error };
}

function TastingPicker({
  catalogue,
  error,
  selectedId,
  onSelect,
}: {
  catalogue?: TastingReadingCatalogue;
  error?: string;
  selectedId: string;
  onSelect: (id: string) => void;
}) {
  const selectId = useId();
  if (error) return <p className="flavor-tasting-note">Tasting readings unavailable: {error}</p>;
  if (!catalogue) return <p className="flavor-tasting-note">Loading tasting readings…</p>;
  const valid = catalogue.readings.filter(isTastingReading);
  const broken = catalogue.readings.filter((entry) => !isTastingReading(entry));
  return (
    <div className="flavor-tasting-picker">
      {valid.length === 0 ? (
        <p className="flavor-tasting-note">
          No tasting readings yet. Run <code>tools/jev_tasting_notes.py read</code> on a capture
          in <code>assets/real_shots/</code> that has tasting notes.
        </p>
      ) : (
        <>
          <label htmlFor={selectId}>Compare with tasting notes</label>
          <select id={selectId} value={selectedId} onChange={(event) => onSelect(event.target.value)}>
            <option value="">None</option>
            {valid.map((reading) => (
              <option key={reading.id} value={reading.id}>
                {reading.capture_id} · {reading.backend === "local" ? "local model" : "Jev"}
              </option>
            ))}
          </select>
        </>
      )}
      {broken.length > 0 && (
        <p
          className="flavor-tasting-note"
          title={broken.map((entry) => ("error" in entry ? entry.error.message : "")).join("\n")}
        >
          {broken.length} reading file{broken.length === 1 ? "" : "s"} could not be read.
        </p>
      )}
    </div>
  );
}

function TastingSummary({ flavor, reading }: { flavor: FlavorResult; reading: TastingReading }) {
  const compared = SENSORY_AXES.filter((axis) => reading.axes[axis].level !== null);
  const agreeing = compared.filter(
    (axis) => levelOfIntensity(flavor.axes[axis].intensity) === reading.axes[axis].level,
  );
  return (
    <div className="flavor-tasting-summary">
      <div className="flavor-verdict">
        {reading.verdict.value ? TASTED_VERDICT_LABEL[reading.verdict.value] : "Taster: verdict unsure"}
      </div>
      <div className="flavor-dominant">
        estimate inside the tasted band on {agreeing.length} of {compared.length} axes
        {compared.length < SENSORY_AXES.length &&
          ` (${SENSORY_AXES.length - compared.length} not mentioned or unsure)`}
      </div>
      {reading.channeling_reported_probability >= 0.5 && (
        <div className="flavor-dominant">channeling reported</div>
      )}
      {/* Said here, not left to docs: a reader must see this is a model's
          reading of someone's notes before comparing it with anything. */}
      <p className="flavor-caveat">
        Shaded bands are an AI reading ({reading.backend === "local" ? "local model " : ""}
        {reading.model.replace(/^local:/, "")}) of the taster's own notes for {reading.capture_id}: not a measurement,
        and never fed back into the solver. Each band is one of five equal steps on the 0–10
        scale. The estimate is relative to this bean's target; the taster is not.
      </p>
    </div>
  );
}

export function FlavorPanel({ flavor }: { flavor: FlavorResult }) {
  const { catalogue, error } = useTastingReadings();
  const [selectedId, setSelectedId] = useState("");
  const reading = catalogue?.readings
    .filter(isTastingReading)
    .find((entry) => entry.id === selectedId);

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

      <TastingPicker
        catalogue={catalogue}
        error={error}
        selectedId={selectedId}
        onSelect={setSelectedId}
      />

      <div className={`flavor-axes${reading ? " with-tasting" : ""}`}>
        {SENSORY_AXES.map((axis) => (
          <AxisBar
            key={axis}
            axis={axis}
            intensity={flavor.axes[axis].intensity}
            target={flavor.axes[axis].target}
            tasted={reading?.axes[axis]}
          />
        ))}
      </div>

      {reading && <TastingSummary flavor={flavor} reading={reading} />}

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
