import type { ComponentProps } from "react";
import { fireEvent, render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";

import { makeCatalogue, makePsdRecipe, makeRecipe } from "../../test/fixtures/recipe";
import { ControlRail } from "./ControlRail";

function renderRail(overrides: Partial<ComponentProps<typeof ControlRail>> = {}) {
  const onChange = vi.fn();
  const onSelectRecipe = vi.fn();
  const onRun = vi.fn();
  const props = {
    recipes: makeCatalogue(),
    selectedId: "baseline",
    onSelectRecipe,
    recipe: makeRecipe(),
    onChange,
    issues: [],
    running: false,
    onRun,
    ...overrides,
  };
  const view = render(<ControlRail {...props} />);
  return { onChange, onSelectRecipe, onRun, props, ...view };
}

describe("ControlRail: number field editing", () => {
  it("labels every puck field so getByLabelText resolves it, and edits produce an immutable patch", () => {
    // ControlRail is fully controlled by its `recipe` prop and owns no state
    // of its own, so simulating several keystrokes (clear + type) fights
    // React's controlled-input reset between events unless the harness
    // re-renders with the patched recipe after every one, exactly as App
    // does. A single native change event is the direct way to exercise the
    // field's own onChange wiring without re-implementing App's loop here.
    const { onChange, props } = renderRail();
    const doseField = screen.getByLabelText(/dose \(g\)/i);
    fireEvent.change(doseField, { target: { value: "20" } });

    expect(onChange).toHaveBeenCalledTimes(1);
    const patched = onChange.mock.calls[0][0];
    // The original recipe object must not have been mutated in place.
    expect(props.recipe.puck.dose_g).toBe(18);
    expect(patched).not.toBe(props.recipe);
    expect(patched.puck).not.toBe(props.recipe.puck);
    expect(patched.puck.dose_g).toBe(20);
    // Sibling fields survive the patch untouched.
    expect(patched.puck.basket_diameter_mm).toBe(props.recipe.puck.basket_diameter_mm);
  });

  it("marks a field invalid via aria-invalid and a CSS class when its path has an issue", () => {
    renderRail({ issues: [{ code: "OUT_OF_RANGE", path: "recipe.puck.dose_g", message: "too low" }] });
    const doseField = screen.getByLabelText(/dose \(g\)/i);
    expect(doseField).toHaveAttribute("aria-invalid", "true");
    expect(doseField).toHaveClass("invalid");
    expect(screen.getByLabelText(/basket/i)).toHaveAttribute("aria-invalid", "false");
  });
});

describe("ControlRail: scalar vs. PSD grind mode", () => {
  it("shows scalar particle diameter and spread controls for a scalar recipe", () => {
    renderRail({ recipe: makeRecipe() });
    expect(screen.getByLabelText(/particle ⌀/i)).toBeInTheDocument();
    expect(screen.getByLabelText(/spread factor/i)).toBeInTheDocument();
  });

  it("renders a read-only PSD readout instead of scalar controls for a PSD recipe", () => {
    renderRail({ recipe: makePsdRecipe() });
    expect(screen.queryByLabelText(/particle ⌀/i)).not.toBeInTheDocument();
    expect(screen.queryByLabelText(/spread factor/i)).not.toBeInTheDocument();
    expect(screen.getByText("150 µm")).toBeInTheDocument();
    expect(screen.getByText("10.0%")).toBeInTheDocument();
  });
});

describe("ControlRail: recipe selection and run button", () => {
  it("preserves a time-only recipe and can enable or disable the target-mass stop", async () => {
    const user = userEvent.setup();
    const timeOnly = makeRecipe();
    timeOnly.stop.target_beverage_g = null;
    const { onChange } = renderRail({ recipe: timeOnly });

    const toggle = screen.getByRole("checkbox", { name: /stop at target mass/i });
    expect(toggle).not.toBeChecked();
    expect(screen.queryByRole("spinbutton", { name: /target mass/i })).not.toBeInTheDocument();

    await user.click(toggle);
    expect(onChange).toHaveBeenCalledWith(expect.objectContaining({
      stop: expect.objectContaining({ target_beverage_g: 36 }),
    }));
  });

  it("restores a custom target mass after temporarily disabling the condition", async () => {
    const user = userEvent.setup();
    const recipe = makeRecipe();
    recipe.stop.target_beverage_g = 42;
    const { onChange, props, rerender } = renderRail({ recipe });

    await user.click(screen.getByRole("checkbox", { name: /stop at target mass/i }));
    expect(onChange).toHaveBeenLastCalledWith(expect.objectContaining({
      stop: expect.objectContaining({ target_beverage_g: null }),
    }));

    const timeOnly = { ...recipe, stop: { ...recipe.stop, target_beverage_g: null } };
    rerender(<ControlRail {...props} recipe={timeOnly} />);
    await user.click(screen.getByRole("checkbox", { name: /stop at target mass/i }));
    expect(onChange).toHaveBeenLastCalledWith(expect.objectContaining({
      stop: expect.objectContaining({ target_beverage_g: 42 }),
    }));
  });

  it("disables the run button while running", () => {
    renderRail({ running: true });
    const button = screen.getByRole("button", { name: /simulating/i });
    expect(button).toBeDisabled();
  });

  it("disables the run button when there are validation issues, independent of running state", () => {
    renderRail({ issues: [{ code: "OUT_OF_RANGE", path: "recipe.puck.dose_g", message: "x" }] });
    expect(screen.getByRole("button", { name: /run simulation/i })).toBeDisabled();
    expect(screen.getByText(/1 input is outside the supported range/i)).toBeInTheDocument();
  });

  it("pluralises the issue count message", () => {
    renderRail({
      issues: [
        { code: "OUT_OF_RANGE", path: "a", message: "x" },
        { code: "OUT_OF_RANGE", path: "b", message: "y" },
      ],
    });
    expect(screen.getByText(/2 inputs are outside the supported range/i)).toBeInTheDocument();
  });

  it("renders a disabled option for a malformed catalogue entry and calls onSelectRecipe for a valid one", async () => {
    const user = userEvent.setup();
    const { onSelectRecipe } = renderRail();
    const brokenOption = screen.getByRole("option", { name: /broken.*failed to load/i });
    expect(brokenOption).toBeDisabled();

    await user.selectOptions(screen.getByRole("combobox", { name: "Recipe" }), "coarse");
    expect(onSelectRecipe).toHaveBeenCalledWith("coarse");
  });

  it("calls onRun when the run button is clicked", async () => {
    const user = userEvent.setup();
    const { onRun } = renderRail();
    await user.click(screen.getByRole("button", { name: /run simulation/i }));
    expect(onRun).toHaveBeenCalledTimes(1);
  });
});
