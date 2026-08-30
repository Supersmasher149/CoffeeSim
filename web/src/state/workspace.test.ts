import { describe, expect, it } from "vitest";

import { makePsdRecipe, makeRecipe } from "../test/fixtures/recipe";
import { inputRanges, localValidation, preInfusionEnd } from "./workspace";

describe("localValidation", () => {
  it("accepts a recipe with every field at its exact low and high boundary", () => {
    const [doseLow, doseHigh] = inputRanges.dose_g;
    expect(localValidation(makeRecipe({ puck: { ...makeRecipe().puck, dose_g: doseLow } }))).toEqual([]);
    expect(localValidation(makeRecipe({ puck: { ...makeRecipe().puck, dose_g: doseHigh } }))).toEqual([]);
  });

  it("accepts the unmodified fallback-shaped recipe with no issues", () => {
    expect(localValidation(makeRecipe())).toEqual([]);
  });

  const scalarRangeCases: [string, readonly [number, number], (v: number) => object][] = [
    ["dose_g", inputRanges.dose_g, (v) => ({ puck: { ...makeRecipe().puck, dose_g: v } })],
    [
      "basket_diameter_mm",
      inputRanges.basket_diameter_mm,
      (v) => ({ puck: { ...makeRecipe().puck, basket_diameter_mm: v } }),
    ],
    ["depth_mm", inputRanges.depth_mm, (v) => ({ puck: { ...makeRecipe().puck, depth_mm: v } })],
    [
      "particle_diameter_um",
      inputRanges.particle_diameter_um,
      (v) => ({ puck: { ...makeRecipe().puck, particle_diameter_um: v } }),
    ],
    [
      "particle_spread_factor",
      inputRanges.particle_spread_factor,
      (v) => ({ puck: { ...makeRecipe().puck, particle_spread_factor: v } }),
    ],
    ["maximum_time_s", inputRanges.maximum_time_s, (v) => ({ stop: { ...makeRecipe().stop, maximum_time_s: v } })],
    [
      "target_beverage_g",
      inputRanges.target_beverage_g,
      (v) => ({ stop: { ...makeRecipe().stop, target_beverage_g: v } }),
    ],
  ];

  for (const [field, [low, high], patch] of scalarRangeCases) {
    describe(field, () => {
      it("is valid exactly at its low boundary", () => {
        const issues = localValidation(makeRecipe(patch(low) as never));
        expect(issues.filter((issue) => issue.path.endsWith(field))).toEqual([]);
      });
      it("is valid exactly at its high boundary", () => {
        const issues = localValidation(makeRecipe(patch(high) as never));
        expect(issues.filter((issue) => issue.path.endsWith(field))).toEqual([]);
      });
      it("flags just below its low boundary", () => {
        const step = (high - low) / 1000 || 0.001;
        const issues = localValidation(makeRecipe(patch(low - step) as never));
        expect(issues.some((issue) => issue.path.endsWith(field) && issue.code === "OUT_OF_RANGE")).toBe(true);
      });
      it("flags just above its high boundary", () => {
        const step = (high - low) / 1000 || 0.001;
        const issues = localValidation(makeRecipe(patch(high + step) as never));
        expect(issues.some((issue) => issue.path.endsWith(field) && issue.code === "OUT_OF_RANGE")).toBe(true);
      });
      it("flags NaN", () => {
        const issues = localValidation(makeRecipe(patch(NaN) as never));
        expect(issues.some((issue) => issue.path.endsWith(field))).toBe(true);
      });
      it("flags positive infinity", () => {
        const issues = localValidation(makeRecipe(patch(Number.POSITIVE_INFINITY) as never));
        expect(issues.some((issue) => issue.path.endsWith(field))).toBe(true);
      });
      it("flags negative infinity", () => {
        const issues = localValidation(makeRecipe(patch(Number.NEGATIVE_INFINITY) as never));
        expect(issues.some((issue) => issue.path.endsWith(field))).toBe(true);
      });
    });
  }

  it("skips the scalar particle fields entirely when the recipe carries a PSD", () => {
    const recipe = makePsdRecipe();
    // Garbage scalar values alongside a PSD must not be checked -- the two
    // spellings are mutually exclusive and the PSD is what is authored.
    const withGarbage = {
      ...recipe,
      puck: { ...recipe.puck, particle_diameter_um: -1, particle_spread_factor: NaN },
    };
    const issues = localValidation(withGarbage);
    expect(issues.some((issue) => issue.path.includes("particle_diameter_um"))).toBe(false);
    expect(issues.some((issue) => issue.path.includes("particle_spread_factor"))).toBe(false);
  });

  it("range-checks the scalar particle fields when no PSD is present", () => {
    const recipe = makeRecipe({ puck: { ...makeRecipe().puck, particle_diameter_um: 5000 } });
    const issues = localValidation(recipe);
    expect(issues.some((issue) => issue.path === "recipe.puck.particle_diameter_um")).toBe(true);
  });

  it("does not range-check a null target beverage mass", () => {
    const recipe = makeRecipe({ stop: { target_beverage_g: null, maximum_time_s: 45 } });
    expect(localValidation(recipe)).toEqual([]);
  });

  describe("pressure profile ordering", () => {
    it("accepts an empty profile with no ordering issues", () => {
      const recipe = makeRecipe({ pressure_profile_bar: [] });
      expect(localValidation(recipe).some((issue) => issue.code === "UNORDERED_PROFILE")).toBe(false);
    });

    it("accepts a single-point profile", () => {
      const recipe = makeRecipe({ pressure_profile_bar: [[0, 6]] });
      expect(localValidation(recipe).some((issue) => issue.code === "UNORDERED_PROFILE")).toBe(false);
    });

    it("flags a duplicate time as unordered", () => {
      const recipe = makeRecipe({
        pressure_profile_bar: [
          [0, 2],
          [5, 4],
          [5, 6],
        ],
      });
      const issues = localValidation(recipe);
      expect(issues.some((issue) => issue.code === "UNORDERED_PROFILE")).toBe(true);
    });

    it("flags a descending profile at every offending index", () => {
      const recipe = makeRecipe({
        pressure_profile_bar: [
          [10, 2],
          [5, 4],
          [0, 6],
        ],
      });
      const issues = localValidation(recipe).filter((issue) => issue.code === "UNORDERED_PROFILE");
      expect(issues).toHaveLength(2);
      expect(issues[0].path).toBe("recipe.pressure_profile_bar[1]");
      expect(issues[1].path).toBe("recipe.pressure_profile_bar[2]");
    });

    it("range-checks every point's value, not only the first", () => {
      const recipe = makeRecipe({
        pressure_profile_bar: [
          [0, 2],
          [10, 200],
        ],
      });
      const issues = localValidation(recipe);
      expect(issues.some((issue) => issue.path === "recipe.pressure_profile_bar[1]")).toBe(true);
    });
  });

  describe("temperature profile ordering", () => {
    it("flags an unordered temperature profile independently of the pressure profile", () => {
      const recipe = makeRecipe({
        temperature_profile_c: [
          [0, 93],
          [0, 94],
        ],
      });
      const issues = localValidation(recipe);
      expect(issues.some((issue) => issue.path === "recipe.temperature_profile_c[1]")).toBe(true);
    });
  });
});

describe("preInfusionEnd", () => {
  it("is undefined for an empty profile", () => {
    expect(preInfusionEnd(makeRecipe({ pressure_profile_bar: [] }))).toBeUndefined();
  });

  it("is undefined for a single-point profile", () => {
    expect(preInfusionEnd(makeRecipe({ pressure_profile_bar: [[0, 6]] }))).toBeUndefined();
  });

  it("is undefined for a flat profile (peak reached at t=0)", () => {
    const recipe = makeRecipe({
      pressure_profile_bar: [
        [0, 9],
        [30, 9],
      ],
    });
    expect(preInfusionEnd(recipe)).toBeUndefined();
  });

  it("is undefined when the peak is hit immediately", () => {
    const recipe = makeRecipe({
      pressure_profile_bar: [
        [0, 9],
        [10, 9],
        [30, 9],
      ],
    });
    expect(preInfusionEnd(recipe)).toBeUndefined();
  });

  it("returns the first time the peak is reached when it repeats later", () => {
    const recipe = makeRecipe({
      pressure_profile_bar: [
        [0, 2],
        [5, 9],
        [10, 9],
        [20, 9],
      ],
    });
    expect(preInfusionEnd(recipe)).toBe(5);
  });

  it("returns the ramp's peak time for an ordinary ramp profile", () => {
    const recipe = makeRecipe({
      pressure_profile_bar: [
        [0, 2],
        [6, 2],
        [10, 9],
        [30, 9],
      ],
    });
    expect(preInfusionEnd(recipe)).toBe(10);
  });
});
