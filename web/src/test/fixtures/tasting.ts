import { SENSORY_AXES } from "../../api/types";
import type { TastingReading, TastingReadingCatalogue, TastingReadingEntry } from "../../api/types";

export function makeTastingReading(overrides: Partial<TastingReading> = {}): TastingReading {
  return {
    id: "2026-10-20-test-01.tasting.local",
    capture_id: "2026-10-20-test-01",
    backend: "local",
    model: "local:qwen2.5:7b-instruct",
    retrieved_at: "2026-10-20T16:00:00Z",
    min_confidence: 0.5,
    axes: Object.fromEntries(
      SENSORY_AXES.map((axis) => [axis, { level: null, status: "not_mentioned", confidence: 0.99 }]),
    ) as TastingReading["axes"],
    verdict: { value: "under_extracted_sour", status: "read", confidence: 0.97 },
    channeling_reported_probability: 0.02,
    ...overrides,
  };
}

export function makeTastingCatalogue(readings: TastingReadingEntry[] = []): TastingReadingCatalogue {
  return { schema_version: "1.0", readings, count: readings.length };
}
