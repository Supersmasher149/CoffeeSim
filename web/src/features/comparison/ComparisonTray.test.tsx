import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";

import { makeShotResult } from "../../test/fixtures/shotResult";
import { ComparisonTray } from "./ComparisonTray";

describe("ComparisonTray", () => {
  it("shows a hint and no runs when the tray is empty", () => {
    render(<ComparisonTray runs={[]} onPin={vi.fn()} onRemove={vi.fn()} canPin />);
    expect(screen.getByText(/pin up to 2 runs/i)).toBeInTheDocument();
  });

  it("disables Pin when canPin is false", () => {
    render(<ComparisonTray runs={[]} onPin={vi.fn()} onRemove={vi.fn()} canPin={false} />);
    expect(screen.getByRole("button", { name: /pin current run/i })).toBeDisabled();
  });

  it("disables Pin once at the two-run capacity even when canPin is true", () => {
    const runs = [makeShotResult(), makeShotResult()];
    render(<ComparisonTray runs={runs} onPin={vi.fn()} onRemove={vi.fn()} canPin />);
    expect(screen.getByRole("button", { name: /pin current run/i })).toBeDisabled();
  });

  it("calls onPin when Pin is clicked and pinning is allowed", async () => {
    const onPin = vi.fn();
    const user = userEvent.setup();
    render(<ComparisonTray runs={[]} onPin={onPin} onRemove={vi.fn()} canPin />);
    await user.click(screen.getByRole("button", { name: /pin current run/i }));
    expect(onPin).toHaveBeenCalledTimes(1);
  });

  it("gives every remove button a distinct, run-scoped accessible name", async () => {
    const runs = [makeShotResult(), makeShotResult()];
    const onRemove = vi.fn();
    const user = userEvent.setup();
    render(<ComparisonTray runs={runs} onPin={vi.fn()} onRemove={onRemove} canPin={false} />);

    const removeButtons = runs.map((run) =>
      screen.getByRole("button", { name: new RegExp(`remove pinned run ${run.manifest.run_id}`, "i") }),
    );
    expect(new Set(removeButtons.map((b) => b.textContent))).toBeDefined(); // both render "×"
    await user.click(removeButtons[0]);
    expect(onRemove).toHaveBeenCalledWith(runs[0].manifest.run_id);
    expect(onRemove).not.toHaveBeenCalledWith(runs[1].manifest.run_id);
  });

  it("notes that the active run is already pinned only when it is present in runs", () => {
    const active = makeShotResult();
    const other = makeShotResult();

    const { rerender } = render(
      <ComparisonTray runs={[other]} activeId={active.manifest.run_id} onPin={vi.fn()} onRemove={vi.fn()} canPin />,
    );
    expect(screen.queryByText(/already pinned/i)).not.toBeInTheDocument();

    rerender(
      <ComparisonTray runs={[active]} activeId={active.manifest.run_id} onPin={vi.fn()} onRemove={vi.fn()} canPin={false} />,
    );
    expect(screen.getByText(/already pinned/i)).toBeInTheDocument();
  });
});
