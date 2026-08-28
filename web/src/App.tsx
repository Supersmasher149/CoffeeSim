import { useCallback, useEffect, useState } from "react";

import { ApiFailure, api, type HealthResponse } from "./api/client";
import type { Recipe, RecipeCatalogueEntry, ReferenceCatalogue, ShotResult } from "./api/types";
import { MeasuredShotComparison } from "./features/calibration/MeasuredShotComparison";
import { ComparisonTray } from "./features/comparison/ComparisonTray";
import { ReferenceShotsPanel } from "./features/references/ReferenceShotsPanel";
import { ChartStack } from "./features/shot/ChartStack";
import { ControlRail } from "./features/shot/ControlRail";
import { DiagnosticsDrawer } from "./features/shot/DiagnosticsDrawer";
import { MetricStrip } from "./features/shot/MetricStrip";
import { PuckView } from "./features/shot/PuckView";
import { SweepPanel } from "./features/sweeps/SweepPanel";
import {
  fallbackRecipe,
  localValidation,
  preInfusionEnd,
  type ShotWorkspace,
} from "./state/workspace";

// Audit P4, issue #18: narrows a RecipeCatalogueEntry so callers can filter
// out {id, error} entries and still have TypeScript know `.recipe` exists on
// what's left, instead of the plain `"recipe" in entry` check widening back
// to the union.
function hasRecipe(
  entry: RecipeCatalogueEntry,
): entry is Extract<RecipeCatalogueEntry, { recipe: Recipe }> {
  return "recipe" in entry;
}

export function App() {
  const [health, setHealth] = useState<HealthResponse>();
  const [catalogue, setCatalogue] = useState<RecipeCatalogueEntry[]>([]);
  const [selectedId, setSelectedId] = useState("baseline");
  const [error, setError] = useState<string>();
  const [referenceError, setReferenceError] = useState<string>();
  const [referenceCatalogue, setReferenceCatalogue] = useState<ReferenceCatalogue>();
  const [pinned, setPinned] = useState<ShotResult[]>([]);

  const [workspace, setWorkspace] = useState<ShotWorkspace>({
    draftRecipe: fallbackRecipe,
    validation: [],
    comparisonRunIds: [],
    requestState: "idle",
  });

  useEffect(() => {
    api.health().then(setHealth).catch(() =>
      setError(
        "Cannot reach the tool server. Start it with " +
          "`./build/apps/espressolab_server/espressolab_server --assets assets`.",
      ),
    );
    api
      .recipes()
      .then((body) => {
        setCatalogue(body.recipes);
        // Audit P4, issue #18: a malformed asset's {id, error} entry has no
        // `recipe`, so the initial selection must skip it rather than seed
        // the workspace with an undefined draft recipe.
        const loaded = body.recipes.filter(hasRecipe);
        const first = loaded.find((entry) => entry.id === "baseline") ?? loaded[0];
        if (first) {
          setSelectedId(first.id);
          setWorkspace((current) => ({ ...current, draftRecipe: first.recipe }));
        }
      })
      .catch(() => undefined);
    api
      .referenceShots()
      .then(setReferenceCatalogue)
      .catch((failure) =>
        setReferenceError(failure instanceof Error ? failure.message : String(failure)),
      );
  }, []);

  const setRecipe = useCallback((recipe: Recipe) => {
    setWorkspace((current) => ({
      ...current,
      draftRecipe: recipe,
      validation: localValidation(recipe),
    }));
  }, []);

  const selectRecipe = (id: string) => {
    const entry = catalogue.find((candidate) => candidate.id === id);
    // Audit P4, issue #18: an {id, error} entry has no `recipe`. ControlRail
    // already renders it as a disabled, unselectable option; this is the
    // defensive second check so a selection can never dereference undefined.
    if (!entry || !hasRecipe(entry)) return;
    setSelectedId(id);
    setRecipe(entry.recipe);
  };

  const run = async () => {
    setError(undefined);
    setWorkspace((current) => ({ ...current, requestState: "running" }));
    try {
      const result = await api.simulate(workspace.draftRecipe);
      setWorkspace((current) => ({ ...current, activeRun: result, requestState: "idle" }));
    } catch (failure) {
      // The server's validation is authoritative; the rail's own check only
      // avoids obviously doomed round trips.
      if (failure instanceof ApiFailure) {
        setWorkspace((current) => ({
          ...current,
          validation: failure.issues,
          requestState: "failed",
        }));
        setError(`${failure.code}: ${failure.message}`);
      } else {
        setError(failure instanceof Error ? failure.message : String(failure));
        setWorkspace((current) => ({ ...current, requestState: "failed" }));
      }
    }
  };

  const active = workspace.activeRun;
  const comparisons = pinned.filter((run_) => run_.manifest.run_id !== active?.manifest.run_id);

  return (
    <div className="app">
      <header className="topbar">
        <h1>EspressoLab</h1>
        <span className="sub">
          {health
            ? `${health.solver_version} · recipe schema ${health.recipe_schema_version}`
            : "connecting to the tool server…"}
        </span>
        <span className="spacer" />
        {active && (
          <>
            <span className="sub">run {active.manifest.run_id}</span>
            <a className="button ghost" href={api.csvUrl(active.manifest.run_id)} download={`${active.manifest.run_id}.csv`}>
              Download CSV
            </a>
            <button
              className="ghost"
              onClick={() => {
                const blob = new Blob([JSON.stringify(active, null, 2)], {
                  type: "application/json",
                });
                const url = URL.createObjectURL(blob);
                const anchor = document.createElement("a");
                anchor.href = url;
                anchor.download = `${active.manifest.run_id}.json`;
                anchor.click();
                URL.revokeObjectURL(url);
              }}
            >
              Download JSON
            </button>
          </>
        )}
      </header>

      <ControlRail
        recipes={catalogue}
        selectedId={selectedId}
        onSelectRecipe={selectRecipe}
        recipe={workspace.draftRecipe}
        onChange={setRecipe}
        issues={workspace.validation}
        running={workspace.requestState === "running"}
        onRun={run}
      />

      <main className="main">
        {error && <div className="error">{error}</div>}

        {workspace.validation.length > 0 && (
          <div className="error">
            {workspace.validation.map((issue) => (
              <div key={issue.path}>
                <code>{issue.code}</code> {issue.path}: {issue.message}
              </div>
            ))}
          </div>
        )}

        {active ? (
          <>
            <MetricStrip result={active} />
            <PuckView
              result={active}
              targetBeverageG={workspace.draftRecipe.stop.target_beverage_g}
              cursorTimeSeconds={workspace.cursorTimeSeconds}
            />
            <ChartStack
              result={active}
              comparisons={comparisons}
              preInfusionEnd={preInfusionEnd(workspace.draftRecipe)}
              onCursorChange={(time) =>
                setWorkspace((current) => ({ ...current, cursorTimeSeconds: time }))
              }
            />
            <ComparisonTray
              runs={pinned}
              activeId={active.manifest.run_id}
              canPin={!pinned.some((run_) => run_.manifest.run_id === active.manifest.run_id)}
              onPin={() => setPinned((current) => [...current, active])}
              onRemove={(runId) =>
                setPinned((current) => current.filter((run_) => run_.manifest.run_id !== runId))
              }
            />
            <DiagnosticsDrawer result={active} />
          </>
        ) : (
          <p className="note" style={{ maxWidth: 620 }}>
            Choose a recipe, adjust the controls, and run the simulation. Every result is produced
            by the native solver and carries its own reproducibility hash — the dashboard performs
            no calculations of its own.
          </p>
        )}

        <MeasuredShotComparison />

        <ReferenceShotsPanel
          catalogue={referenceCatalogue}
          error={referenceError}
          active={active}
          recipe={workspace.draftRecipe}
        />

        <SweepPanel
          baseline={workspace.draftRecipe}
          parameters={health?.sweepable_parameters ?? ["puck.particle_diameter_um"]}
          onError={setError}
        />
      </main>
    </div>
  );
}
