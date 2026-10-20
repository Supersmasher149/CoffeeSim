import { describe, expect, it } from "vitest";

import { isTastingReading, levelOfIntensity, tastingBand } from "./tasting";
import { makeTastingReading } from "../../test/fixtures/tasting";

describe("tasting bands", () => {
  // The same cut-offs tests/tools/jev_tasting_notes_check.py pins for the
  // terminal `compare`, so the dashboard and the tool can never disagree.
  it.each([
    [0, "absent"],
    [1.99, "absent"],
    [2, "faint"],
    [5.8, "moderate"],
    [7.61, "pronounced"],
    [9.35, "dominant"],
    [10, "dominant"],
  ])("bands overlay intensity %s as %s", (intensity, level) => {
    expect(levelOfIntensity(intensity)).toBe(level);
  });

  it("clamps out-of-scale intensities into the end bands", () => {
    expect(levelOfIntensity(-0.5)).toBe("absent");
    expect(levelOfIntensity(10.5)).toBe("dominant");
  });

  it("places each tasted level on a two-point slice of the 0-10 bar", () => {
    expect(tastingBand("absent")).toEqual([0, 2]);
    expect(tastingBand("moderate")).toEqual([4, 6]);
    expect(tastingBand("dominant")).toEqual([8, 10]);
  });

  it("tells a reading from an error entry", () => {
    expect(isTastingReading(makeTastingReading())).toBe(true);
    expect(isTastingReading({ id: "x", error: { code: "TASTING_READING_INVALID", message: "m" } })).toBe(
      false,
    );
  });
});
