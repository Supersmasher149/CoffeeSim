// Audit #06: measured shots and published references used to be two tabs
// with two pickers for the same question ("what does the real world say?").
// They now share one "Calibration" workflow and one ground-truth list.
export type WorkflowId = "shot" | "calibration" | "sweeps";

const workflows: { id: WorkflowId; label: string; description: string }[] = [
  { id: "shot", label: "Shot", description: "Author a recipe and inspect a native simulation" },
  { id: "calibration", label: "Calibration", description: "Compare a measured shot or a published reference against a simulation" },
  { id: "sweeps", label: "Sweeps", description: "Explore parameter sensitivity" },
];

interface Props {
  active: WorkflowId;
  onChange: (workflow: WorkflowId) => void;
}

export function WorkflowTabs({ active, onChange }: Props) {
  const move = (index: number, direction: number) => {
    const next = (index + direction + workflows.length) % workflows.length;
    const workflow = workflows[next];
    onChange(workflow.id);
    document.getElementById(`workflow-tab-${workflow.id}`)?.focus();
  };

  return (
    <nav className="workflow-nav" aria-label="Workbench workflows">
      <div className="workflow-tabs" role="tablist" aria-label="Workbench workflows">
        {workflows.map((workflow, index) => (
          <button
            key={workflow.id}
            id={`workflow-tab-${workflow.id}`}
            className="workflow-tab"
            type="button"
            role="tab"
            aria-selected={active === workflow.id}
            aria-controls={`workflow-panel-${workflow.id}`}
            tabIndex={active === workflow.id ? 0 : -1}
            title={workflow.description}
            onClick={() => onChange(workflow.id)}
            onKeyDown={(event) => {
              if (event.key === "ArrowRight") {
                event.preventDefault();
                move(index, 1);
              } else if (event.key === "ArrowLeft") {
                event.preventDefault();
                move(index, -1);
              } else if (event.key === "Home") {
                event.preventDefault();
                onChange(workflows[0].id);
                document.getElementById(`workflow-tab-${workflows[0].id}`)?.focus();
              } else if (event.key === "End") {
                event.preventDefault();
                const last = workflows[workflows.length - 1];
                onChange(last.id);
                document.getElementById(`workflow-tab-${last.id}`)?.focus();
              }
            }}
          >
            <span>{workflow.label}</span>
          </button>
        ))}
      </div>
    </nav>
  );
}
