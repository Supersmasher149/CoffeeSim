import type { MeasuredShotCatalogue, ReferenceCatalogue, ReferenceRecord } from "../../api/types";

export type GroundTruthSelection = { kind: "measured"; id: string } | { kind: "reference"; id: string };

interface Props {
  measuredCatalogue?: MeasuredShotCatalogue;
  referenceCatalogue?: ReferenceCatalogue;
  selected?: GroundTruthSelection;
  onSelect: (selection: GroundTruthSelection) => void;
}

function referenceLabel(reference: ReferenceRecord) {
  return reference.id.replace(/^real_gagne_/, "").replace(/_/g, " ").toUpperCase();
}

// Audit #06: measured shots and published references answered the same
// question ("what does the real world say?") from two separate tabs with two
// pickers. One list now carries both, each row declaring what it actually
// covers -- a measured shot has the full paired mass series a comparison
// needs; a reference is metadata only, sometimes for a single channel.
export function GroundTruthList({ measuredCatalogue, referenceCatalogue, selected, onSelect }: Props) {
  const rows: { key: string; selection: GroundTruthSelection; name: string; caption: string }[] = [
    ...(measuredCatalogue?.measured_shots.map((shot) => ({
      key: `measured:${shot.id}`,
      selection: { kind: "measured" as const, id: shot.id },
      name: shot.id,
      caption: `measured · ${shot.synthetic ? "synthetic" : "real"}${shot.machine ? ` · ${shot.machine}` : ""}`,
    })) ?? []),
    ...(referenceCatalogue?.references.map((reference) => ({
      key: `reference:${reference.id}`,
      selection: { kind: "reference" as const, id: reference.id },
      name: referenceLabel(reference),
      caption: "published · metadata only",
    })) ?? []),
  ];

  if (rows.length === 0) return null;

  return (
    <div className="ground-truth-list" aria-label="Ground truth: measured shots and published references">
      {rows.map((row) => {
        const isSelected =
          selected?.kind === row.selection.kind && selected.id === row.selection.id;
        return (
          <button
            key={row.key}
            type="button"
            className={`ground-truth-row${isSelected ? " selected" : ""}`}
            aria-pressed={isSelected}
            onClick={() => onSelect(row.selection)}
          >
            <span className="ground-truth-name">{row.name}</span>
            <span className="ground-truth-caption">{row.caption}</span>
          </button>
        );
      })}
    </div>
  );
}
