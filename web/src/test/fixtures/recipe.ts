import type { Recipe, RecipeCatalogueEntry } from "../../api/types";

export function makeRecipe(overrides: Partial<Recipe> = {}): Recipe {
  return {
    schema_version: "1.0",
    name: "Baseline 18 g espresso",
    puck: {
      dose_g: 18,
      basket_diameter_mm: 58,
      depth_mm: 9,
      particle_diameter_um: 350,
      particle_spread_factor: 0.55,
    },
    pressure_profile_bar: [
      [0, 2],
      [6, 2],
      [10, 9],
      [30, 9],
    ],
    temperature_profile_c: [[0, 93]],
    stop: { target_beverage_g: 36, maximum_time_s: 45 },
    ...overrides,
  };
}

export function makePsdRecipe(overrides: Partial<Recipe> = {}): Recipe {
  const recipe = makeRecipe(overrides);
  const { particle_diameter_um: _diameter, particle_spread_factor: _spread, ...puck } = recipe.puck;
  return {
    ...recipe,
    puck: {
      ...puck,
      grind: {
        bins: [
          { diameter_um: 150, mass_fraction: 0.1 },
          { diameter_um: 300, mass_fraction: 0.55 },
          { diameter_um: 600, mass_fraction: 0.35 },
        ],
      },
    },
  };
}

export function makeCatalogue(): RecipeCatalogueEntry[] {
  return [
    { id: "baseline", name: "Baseline 18 g espresso", recipe: makeRecipe() },
    {
      id: "coarse",
      name: "Coarse 18 g espresso",
      recipe: makeRecipe({ name: "Coarse 18 g espresso", puck: { ...makeRecipe().puck, particle_diameter_um: 600 } }),
    },
    { id: "broken", error: { code: "SCHEMA_VIOLATION", message: "puck.dose_g is required" } },
  ];
}
