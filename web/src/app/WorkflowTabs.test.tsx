import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";

import { WorkflowTabs } from "./WorkflowTabs";

describe("WorkflowTabs", () => {
  it("marks the active workflow and reports a clicked workflow", async () => {
    const user = userEvent.setup();
    const onChange = vi.fn();
    render(<WorkflowTabs active="shot" onChange={onChange} />);

    expect(screen.getByRole("tab", { name: "Shot" })).toHaveAttribute("aria-selected", "true");
    await user.click(screen.getByRole("tab", { name: "Sweeps" }));
    expect(onChange).toHaveBeenCalledWith("sweeps");
  });

  it("supports arrow, Home, and End navigation", async () => {
    const user = userEvent.setup();
    const onChange = vi.fn();
    render(<WorkflowTabs active="shot" onChange={onChange} />);
    const shot = screen.getByRole("tab", { name: "Shot" });
    shot.focus();

    await user.keyboard("{ArrowRight}");
    expect(onChange).toHaveBeenLastCalledWith("calibration");
    await user.keyboard("{End}");
    expect(onChange).toHaveBeenLastCalledWith("sweeps");
    await user.keyboard("{Home}");
    expect(onChange).toHaveBeenLastCalledWith("shot");
  });
});
