import { useEffect, useRef, useState } from "react";

import { api, type HealthResponse } from "../api/client";
import type { Recipe, RecipeCatalogueEntry, ReferenceCatalogue } from "../api/types";

export function hasRecipe(
  entry: RecipeCatalogueEntry,
): entry is Extract<RecipeCatalogueEntry, { recipe: Recipe }> {
  return "recipe" in entry;
}

export function useBootstrap(onInitialRecipe: (id: string, recipe: Recipe) => void) {
  const initialRecipe = useRef(onInitialRecipe);
  initialRecipe.current = onInitialRecipe;
  const [health, setHealth] = useState<HealthResponse>();
  const [catalogue, setCatalogue] = useState<RecipeCatalogueEntry[]>([]);
  const [referenceCatalogue, setReferenceCatalogue] = useState<ReferenceCatalogue>();
  const [healthError, setHealthError] = useState<string>();
  const [recipeError, setRecipeError] = useState<string>();
  const [referenceError, setReferenceError] = useState<string>();

  useEffect(() => {
    let active = true;
    api.health().then((response) => active && setHealth(response)).catch(() => {
      if (active) {
        setHealthError(
          "Cannot reach the tool server. Start it with " +
            "`./build/apps/espressolab_server/espressolab_server --assets assets`.",
        );
      }
    });
    api.recipes().then((body) => {
      if (!active) return;
      setCatalogue(body.recipes);
      const loaded = body.recipes.filter(hasRecipe);
      const first = loaded.find((entry) => entry.id === "baseline") ?? loaded[0];
      if (first) initialRecipe.current(first.id, first.recipe);
    }).catch((failure) => {
      if (active) setRecipeError(failure instanceof Error ? failure.message : String(failure));
    });
    api.referenceShots().then((response) => {
      if (active) setReferenceCatalogue(response);
    }).catch((failure) => {
      if (active) setReferenceError(failure instanceof Error ? failure.message : String(failure));
    });
    return () => {
      active = false;
    };
  }, []);

  return {
    health,
    catalogue,
    referenceCatalogue,
    healthError,
    recipeError,
    referenceError,
  };
}
