import { Suspense, lazy, useCallback, useRef, useState } from "react";

import { ApiFailure, api } from "./api/client";
import type { Recipe, ShotResult } from "./api/types";
import { type WorkflowId, WorkflowTabs } from "./app/WorkflowTabs";
import { hasRecipe, useBootstrap } from "./app/useBootstrap";
import { ShotWorkspace } from "./features/shot/ShotWorkspace";
import {
  fallbackRecipe,
  localValidation,
  type ShotWorkspace as ShotWorkspaceState,
} from "./state/workspace";

// Both panels below are recharts consumers (Calibration renders
// MeasuredShotComparison; SweepPanel renders its own heat map and profile
// charts); they're the only two chart panels that render regardless of
// which tab is active (hidden by CSS, not unmounted -- App: workflow
// navigation's "preserves workflow-local state" contract needs them to stay
// mounted once visited). Loading them eagerly would pull recharts back into
// the initial bundle no matter how the Shot tab defers ChartStack, so each
// is its own dynamic import and neither renders until its tab has been
// opened at least once.
const Calibration = lazy(() =>
  import("./features/calibration/Calibration").then((mod) => ({
    default: mod.Calibration,
  })),
);
const SweepPanel = lazy(() =>
  import("./features/sweeps/SweepPanel").then((mod) => ({ default: mod.SweepPanel })),
);

export function App() {
  const [selectedId, setSelectedId] = useState("baseline");
  const [shotError, setShotError] = useState<string>();
  const [sweepError, setSweepError] = useState<string>();
  const [pinned, setPinned] = useState<ShotResult[]>([]);
  const [activeWorkflow, setActiveWorkflow] = useState<WorkflowId>("shot");
  // Tracks every tab that has ever been opened, so the Calibration and
  // Sweeps panels (each `hidden`, not unmounted, once visited -- see the
  // dynamic-import note above) mount their lazy chunk once and then stay
  // mounted, instead of never rendering or re-fetching on every tab switch.
  const [visitedWorkflows, setVisitedWorkflows] = useState<Set<WorkflowId>>(
    () => new Set(["shot"]),
  );
  const changeWorkflow = useCallback((workflow: WorkflowId) => {
    setActiveWorkflow(workflow);
    setVisitedWorkflows((current) =>
      current.has(workflow) ? current : new Set(current).add(workflow),
    );
  }, []);
  const draftTouched = useRef(false);

  const [workspace, setWorkspace] = useState<ShotWorkspaceState>({
    draftRecipe: fallbackRecipe,
    validation: [],
    requestState: "idle",
  });

  const {
    health,
    catalogue,
    referenceCatalogue,
    healthError,
    recipeError,
    referenceError,
  } = useBootstrap((id, recipe) => {
    if (draftTouched.current) return;
    setSelectedId(id);
    setWorkspace((current) => ({ ...current, draftRecipe: recipe }));
  });

  const setRecipe = useCallback((recipe: Recipe) => {
    draftTouched.current = true;
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
    setShotError(undefined);
    // Audit P7, issue #22: snapshot the recipe actually submitted, not
    // workspace.draftRecipe read again after the request resolves -- the
    // user can keep editing the draft while the request is in flight, and
    // the result must stay tied to what produced it, not to draftRecipe's
    // state whenever the response happens to land.
    const submittedRecipe = workspace.draftRecipe;
    setWorkspace((current) => ({ ...current, requestState: "running" }));
    try {
      const result = await api.simulate(submittedRecipe);
      setWorkspace((current) => ({
        ...current,
        activeRun: result,
        activeRecipe: submittedRecipe,
        requestState: "idle",
      }));
    } catch (failure) {
      // The server's validation is authoritative; the rail's own check only
      // avoids obviously doomed round trips.
      if (failure instanceof ApiFailure) {
        setWorkspace((current) => ({
          ...current,
          validation: failure.issues,
          requestState: "failed",
        }));
        setShotError(`${failure.code}: ${failure.message}`);
      } else {
        setShotError(failure instanceof Error ? failure.message : String(failure));
        setWorkspace((current) => ({ ...current, requestState: "failed" }));
      }
    }
  };

  const active = workspace.activeRun;
  return (
    <div className="app">
      <header className="topbar">
        <h1>EspressoLab</h1>
        <span className="sub">
          {health
            ? `${health.solver_version} · recipe schema ${health.recipe_schema_version}`
            : healthError
              ? "tool server unavailable"
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
      <WorkflowTabs active={activeWorkflow} onChange={changeWorkflow} />

      <main className="main" id="main-content">
        {healthError && <div className="error" role="alert">{healthError}</div>}

        <section
          id="workflow-panel-shot"
          role="tabpanel"
          aria-labelledby="workflow-tab-shot"
          hidden={activeWorkflow !== "shot"}
        >
          {recipeError && <div className="error" role="alert">Recipe catalogue unavailable: {recipeError}</div>}
          {shotError && <div className="error" role="alert">{shotError}</div>}
          {workspace.validation.length > 0 && (
            <div className="error" role="alert">
              {workspace.validation.map((issue) => (
                <div key={issue.path}>
                  <code>{issue.code}</code> {issue.path}: {issue.message}
                </div>
              ))}
            </div>
          )}
          <ShotWorkspace
            recipes={catalogue}
            selectedId={selectedId}
            onSelectRecipe={selectRecipe}
            draftRecipe={workspace.draftRecipe}
            onRecipeChange={setRecipe}
            issues={workspace.validation}
            running={workspace.requestState === "running"}
            onRun={run}
            active={active}
            activeRecipe={workspace.activeRecipe}
            pinned={pinned}
            onPin={() => active && setPinned((current) => [...current, active])}
            onRemovePin={(runId) =>
              setPinned((current) => current.filter((run_) => run_.manifest.run_id !== runId))
            }
          />
        </section>

        <section
          id="workflow-panel-calibration"
          className="workflow-panel"
          role="tabpanel"
          aria-labelledby="workflow-tab-calibration"
          hidden={activeWorkflow !== "calibration"}
        >
          <header className="workflow-heading">
            <p className="eyebrow">Measured against simulated</p>
            <h2>Calibration</h2>
            <p>
              Measured shots and published references are both ground truth, so they share one list.
              This workflow never fits coefficients.
            </p>
          </header>
          {visitedWorkflows.has("calibration") && (
            <Suspense fallback={<p className="note" aria-live="polite">Loading calibration workflow...</p>}>
              <Calibration
                referenceCatalogue={referenceCatalogue}
                referenceError={referenceError}
                active={active}
                recipe={workspace.activeRecipe}
              />
            </Suspense>
          )}
        </section>

        <section
          id="workflow-panel-sweeps"
          className="workflow-panel"
          role="tabpanel"
          aria-labelledby="workflow-tab-sweeps"
          hidden={activeWorkflow !== "sweeps"}
        >
          <header className="workflow-heading">
            <p className="eyebrow">Parameter sensitivity</p>
            <h2>Sweep laboratory</h2>
            <p>Run one- or two-axis experiments from the current draft recipe.</p>
          </header>
          {sweepError && <div className="error" role="alert">{sweepError}</div>}
          {visitedWorkflows.has("sweeps") && (
            <Suspense fallback={<p className="note" aria-live="polite">Loading sweep workflow...</p>}>
              <SweepPanel
                baseline={workspace.draftRecipe}
                parameters={health?.sweepable_parameters ?? ["puck.particle_diameter_um"]}
                onError={setSweepError}
              />
            </Suspense>
          )}
        </section>
      </main>
    </div>
  );
}
