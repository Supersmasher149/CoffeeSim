import { useState } from "react";

import type { MeasuredShotCatalogue, Recipe, ReferenceCatalogue, ShotResult } from "../../api/types";
import { ReferenceShotsPanel } from "../references/ReferenceShotsPanel";
import { GroundTruthList, type GroundTruthSelection } from "./GroundTruthList";
import { MeasuredShotComparison } from "./MeasuredShotComparison";

interface Props {
  referenceCatalogue?: ReferenceCatalogue;
  referenceError?: string;
  active?: ShotResult;
  recipe?: Recipe;
}

// Audit #06's merge point: one workflow, one ground-truth list, and whichever
// detail view (measured comparison or reference metadata) the selection
// calls for -- replacing the separate Measured data / References tabs.
export function Calibration({ referenceCatalogue, referenceError, active, recipe }: Props) {
  const [measuredCatalogue, setMeasuredCatalogue] = useState<MeasuredShotCatalogue>();
  const [selection, setSelection] = useState<GroundTruthSelection>();

  const selectedReference =
    selection?.kind === "reference"
      ? referenceCatalogue?.references.find((reference) => reference.id === selection.id)
      : undefined;

  return (
    <div className="calibration">
      {/* A reference-catalogue failure used to be its own tab's problem to
          report; the list that would otherwise show references is now the
          only place left to say so, regardless of what's selected. */}
      {referenceError && (
        <div className="error" role="alert">Reference catalogue unavailable: {referenceError}</div>
      )}
      <GroundTruthList
        measuredCatalogue={measuredCatalogue}
        referenceCatalogue={referenceCatalogue}
        selected={selection}
        onSelect={setSelection}
      />

      {/* Kept mounted (hidden, not unmounted) so its own catalogue fetch and
          comparison state survive switching to look at a reference and back. */}
      <div hidden={selection?.kind === "reference"}>
        <MeasuredShotComparison
          selectedId={selection?.kind === "measured" ? selection.id : undefined}
          onSelectId={(id) => setSelection({ kind: "measured", id })}
          onCatalogueLoaded={setMeasuredCatalogue}
          hidePicker
        />
      </div>

      {selection?.kind === "reference" && referenceCatalogue && (
        <ReferenceShotsPanel
          catalogue={
            selectedReference
              ? { ...referenceCatalogue, references: [selectedReference] }
              : referenceCatalogue
          }
          active={active}
          recipe={recipe}
          error={referenceError}
        />
      )}
    </div>
  );
}
