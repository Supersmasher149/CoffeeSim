// Single source for chart trace color/weight/dash, replacing the four
// independent hardcoded-hex palettes that used to live in ChartStack.tsx,
// HeatMap.tsx, SweepPanel.tsx, and MeasuredShotComparison.tsx (audit "Tokens
// and primitives" sheet + design canvas project e14ffbff). Gold is load-
// bearing and restricted to exactly four uses across the app; the flow trace
// here and the "simulated" trace in Calibration share it deliberately.

export interface TraceStyle {
  color: string;
  width: number;
  dash?: string;
}

export type TraceKey =
  | "pressure"
  | "measured"
  | "flow"
  | "simulated"
  | "mass"
  | "temperature"
  | "residual";

export const TRACE_STYLES: Record<TraceKey, TraceStyle> = {
  pressure: { color: "#f2ece2", width: 2.2 },
  measured: { color: "#f2ece2", width: 2.2 },
  flow: { color: "#d5a25a", width: 2.2, dash: "6 5" },
  simulated: { color: "#d5a25a", width: 2.2, dash: "6 5" },
  mass: { color: "#8f8474", width: 1.6, dash: "5 4" },
  temperature: { color: "#6b6255", width: 1.6, dash: "2 4" },
  residual: { color: "#8a6226", width: 1.5 },
};

// Audit #07: ChartStack.overlay() used to assign palette[index + 1] by
// position in the comparisons array, so unpinning one run silently recolored
// every remaining run. Pinned/overlay runs now keep the same base channel
// color and are told apart by a dash pattern keyed to the run's own id, which
// stays stable regardless of how many other runs are pinned or in what order.
const OVERLAY_DASHES = ["3 3", "7 3 1 3", "1 3", "8 2 2 2", "2 2 6 2"];

export function overlayDashFor(runId: string): string {
  let hash = 0;
  for (let index = 0; index < runId.length; index += 1) {
    hash = (hash * 31 + runId.charCodeAt(index)) >>> 0;
  }
  return OVERLAY_DASHES[hash % OVERLAY_DASHES.length];
}
