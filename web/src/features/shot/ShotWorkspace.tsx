import { Suspense, lazy, useState } from "react";

import type { Recipe, RecipeCatalogueEntry, ShotResult, ValidationIssue } from "../../api/types";
import { preInfusionEnd } from "../../state/workspace";
import { ComparisonTray } from "../comparison/ComparisonTray";
import { ControlRail } from "./ControlRail";
import { DiagnosticsDrawer } from "./DiagnosticsDrawer";
import { FlavorPanel } from "./FlavorPanel";
import { MetricStrip } from "./MetricStrip";
import { PuckView } from "./PuckView";

// recharts (and the d3/decimal.js chain it pulls in) is the single largest
// contributor to the production bundle -- see the bundle-visualizer note in
// vite.config.ts. ChartStack only ever renders once a shot has run, so a
// dynamic import keeps that weight out of the chunk needed just to open the
// workbench and edit a recipe.
const ChartStack = lazy(() => import("./ChartStack").then((mod) => ({ default: mod.ChartStack })));

interface Props {
  recipes: RecipeCatalogueEntry[];
  selectedId: string;
  onSelectRecipe: (id: string) => void;
  draftRecipe: Recipe;
  onRecipeChange: (recipe: Recipe) => void;
  issues: ValidationIssue[];
  running: boolean;
  onRun: () => void;
  active?: ShotResult;
  activeRecipe?: Recipe;
  pinned: ShotResult[];
  onPin: () => void;
  onRemovePin: (runId: string) => void;
}

export function ShotWorkspace({
  recipes,
  selectedId,
  onSelectRecipe,
  draftRecipe,
  onRecipeChange,
  issues,
  running,
  onRun,
  active,
  activeRecipe,
  pinned,
  onPin,
  onRemovePin,
}: Props) {
  const [cursorTimeSeconds, setCursorTimeSeconds] = useState<number>();
  const [editorOpen, setEditorOpen] = useState(false);
  const comparisons = pinned.filter((run) => run.manifest.run_id !== active?.manifest.run_id);

  return (
    <div className={`shot-workspace${editorOpen ? " editor-open" : ""}`}>
      <button
        className="recipe-editor-toggle ghost"
        type="button"
        aria-expanded={editorOpen}
        aria-controls="recipe-editor"
        onClick={() => setEditorOpen((open) => !open)}
      >
        {editorOpen ? "Close recipe editor" : "Edit recipe"}
      </button>
      <div id="recipe-editor" className="recipe-editor">
        <ControlRail
          recipes={recipes}
          selectedId={selectedId}
          onSelectRecipe={onSelectRecipe}
          recipe={draftRecipe}
          onChange={onRecipeChange}
          issues={issues}
          running={running}
          onRun={onRun}
        />
      </div>

      <div className="shot-results">
        {active ? (
          <>
            <MetricStrip result={active} />
            {active.flavor && <FlavorPanel flavor={active.flavor} />}
            <PuckView
              result={active}
              targetBeverageG={activeRecipe?.stop.target_beverage_g ?? null}
              cursorTimeSeconds={cursorTimeSeconds}
            />
            <Suspense fallback={<p className="note" aria-live="polite">Loading charts...</p>}>
              <ChartStack
                result={active}
                comparisons={comparisons}
                preInfusionEnd={activeRecipe ? preInfusionEnd(activeRecipe) : undefined}
                onCursorChange={setCursorTimeSeconds}
              />
            </Suspense>
            <ComparisonTray
              runs={pinned}
              activeId={active.manifest.run_id}
              canPin={!pinned.some((run) => run.manifest.run_id === active.manifest.run_id)}
              onPin={onPin}
              onRemove={onRemovePin}
            />
            <DiagnosticsDrawer result={active} />
          </>
        ) : (
          <div className="empty-state">
            <p className="eyebrow">Native simulation workspace</p>
            <h2>Shape the shot, then inspect every consequence.</h2>
            <p>
              Choose a recipe, adjust the physical inputs, and run the native solver. Results carry
              their own provenance and reproducibility hash; the dashboard does not calculate model outputs.
            </p>
          </div>
        )}
      </div>
    </div>
  );
}
