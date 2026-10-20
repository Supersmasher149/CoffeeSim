import { TASTING_LEVELS } from "../../api/types";
import type { TastingLevel, TastingReading, TastingReadingEntry } from "../../api/types";

// Display-only placement of a tasted level on the overlay's 0-10 bar. These are
// the same five equal bands `tools/jev_tasting_notes.py compare` uses (2 points
// each, top band closed), so the bar and the terminal table always agree. No
// authoritative quantity is derived here: the overlay's intensity is the
// solver's, and the tasted level is the reading's.
export const TASTING_BAND_WIDTH = 10 / TASTING_LEVELS.length;

export function tastingBand(level: TastingLevel): [number, number] {
  const index = TASTING_LEVELS.indexOf(level);
  return [index * TASTING_BAND_WIDTH, (index + 1) * TASTING_BAND_WIDTH];
}

export function levelOfIntensity(intensity: number): TastingLevel {
  const clamped = Math.max(0, Math.min(10, intensity));
  const index = Math.min(Math.floor(clamped / TASTING_BAND_WIDTH), TASTING_LEVELS.length - 1);
  return TASTING_LEVELS[index];
}

export function isTastingReading(entry: TastingReadingEntry): entry is TastingReading {
  return !("error" in entry);
}
