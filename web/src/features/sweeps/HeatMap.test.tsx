import type { ComponentProps } from "react";
import { fireEvent, render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it } from "vitest";

import { makeSweepRow } from "../../test/fixtures/sweep";
import { HeatMap } from "./HeatMap";

const X_VALUES = [250, 300, 350];
const Y_VALUES = [88, 92, 96];

function makeGrid() {
  const rows = [];
  let index = 0;
  for (const y of Y_VALUES) {
    for (const x of X_VALUES) {
      rows.push(
        makeSweepRow({
          index: index++,
          coordinates: [y, x],
          shot_time_s: 20 + x / 50 + y / 10,
          tds_percent: 9,
          extraction_yield_percent: 20,
          beverage_mass_g: 36,
        }),
      );
    }
  }
  return rows;
}

function renderHeatMap(overrides: Partial<ComponentProps<typeof HeatMap>> = {}) {
  const props = {
    rows: makeGrid(),
    xValues: X_VALUES,
    yValues: Y_VALUES,
    xLabel: "puck.particle_diameter_um",
    yLabel: "temperature_profile_c.constant",
    metric: "shot_time_s" as const,
    metricLabel: "shot time",
    metricUnit: "s",
    ...overrides,
  };
  render(<HeatMap {...props} />);
  return props;
}

describe("HeatMap: structure and orientation", () => {
  it("renders one accessible cell button per (x, y) coordinate", () => {
    renderHeatMap();
    const group = screen.getByRole("group", { name: /heat map of shot time/i });
    expect(within(group).getAllByRole("button")).toHaveLength(X_VALUES.length * Y_VALUES.length);
  });

  it("lists the y-axis tick labels from highest to lowest (reversed Y coordinates)", () => {
    renderHeatMap();
    // formatTick renders each y once as a row label; the highest reported
    // value must be the first row rendered, top-down like a plot.
    const rowLabels = screen.getAllByText(/^(88|92|96)$/);
    expect(rowLabels.map((el) => el.textContent)).toEqual(["96", "92", "88"]);
  });

  it("handles a grid where every valid cell shares the same metric value without dividing by zero", () => {
    const flatRows = makeGrid().map((row) => ({ ...row, shot_time_s: 27 }));
    renderHeatMap({ rows: flatRows });
    const group = screen.getByRole("group", { name: /heat map of/i });
    const buttons = within(group).getAllByRole("button");
    expect(buttons.every((button) => /27\.0 s/.test(button.getAttribute("aria-label") ?? ""))).toBe(true);
  });
});

describe("HeatMap: invalid runs", () => {
  it("labels an invalid_state cell as outside the supported range rather than a magnitude", () => {
    const rows = makeGrid();
    rows[0] = { ...rows[0], termination: "invalid_state" };
    renderHeatMap({ rows });
    expect(screen.getByRole("button", { name: /outside the supported input range/i })).toBeInTheDocument();
  });

  it("excludes invalid runs from the min/max range used for the color ramp and legend", () => {
    const rows = makeGrid();
    // Make the first cell's value an extreme outlier, but mark it invalid --
    // the ramp's displayed min/max must come from the *valid* cells only.
    rows[0] = { ...rows[0], termination: "invalid_state", shot_time_s: 999 };
    renderHeatMap({ rows });
    expect(screen.queryByText("999.0")).not.toBeInTheDocument();
  });

  it("disables a button for a coordinate with no run at all", () => {
    const rows = makeGrid().slice(1); // drop the first coordinate entirely
    renderHeatMap({ rows });
    const group = screen.getByRole("group", { name: /heat map of/i });
    const missing = within(group).getByRole("button", { name: /no run/i });
    expect(missing).toBeDisabled();
  });

  it("distinguishes cells skipped by cancellation from invalid solver runs", () => {
    const rows = makeGrid().slice(1);
    renderHeatMap({ rows, partial: true });
    expect(screen.getByRole("button", { name: /not run before cancellation/i })).toBeDisabled();
    expect(screen.getByText("not run before cancellation")).toBeInTheDocument();
    expect(screen.getByText("outside the supported input range")).toBeInTheDocument();
  });
});

describe("HeatMap: live preview vs. sticky pin (audit #09)", () => {
  it("shows the hovered cell's detail as a live preview, falling back to the placeholder with nothing pinned", async () => {
    const user = userEvent.setup();
    renderHeatMap();
    expect(screen.getByText(/click or focus a cell for its run/i)).toBeInTheDocument();

    const cell = screen.getByRole("button", { name: /temperature_profile_c\.constant 88, puck\.particle_diameter_um 250/i });
    await user.hover(cell);
    expect(screen.getByText(/target mass reached/i)).toBeInTheDocument();

    await user.unhover(cell);
    expect(screen.getByText(/click or focus a cell for its run/i)).toBeInTheDocument();
  });

  it("keeps a clicked cell's detail on screen after blur, instead of resetting to the placeholder", async () => {
    const user = userEvent.setup();
    renderHeatMap();
    const cell = screen.getByRole("button", { name: /temperature_profile_c\.constant 96, puck\.particle_diameter_um 350/i });

    await user.click(cell);
    expect(cell).toHaveAttribute("aria-pressed", "true");
    expect(screen.getByText(/target mass reached/i)).toBeInTheDocument();
    expect(screen.getByText(/pinned/i)).toBeInTheDocument();

    fireEvent.blur(cell);
    // Audit #09: this is the concrete fix -- blur used to erase the detail
    // entirely; it now survives because the cell was pinned, not just hovered.
    expect(screen.getByText(/target mass reached/i)).toBeInTheDocument();
    expect(screen.queryByText(/click or focus a cell for its run/i)).not.toBeInTheDocument();
  });

  it("holds a second, shift-clicked cell alongside the pinned one for side-by-side comparison", async () => {
    const user = userEvent.setup();
    renderHeatMap();
    const first = screen.getByRole("button", { name: /temperature_profile_c\.constant 88, puck\.particle_diameter_um 250/i });
    const second = screen.getByRole("button", { name: /temperature_profile_c\.constant 96, puck\.particle_diameter_um 350/i });

    await user.click(first);
    fireEvent.click(second, { shiftKey: true });

    expect(screen.getByText(/pinned/i)).toBeInTheDocument();
    expect(screen.getByText(/held/i)).toBeInTheDocument();
    // Both cells' detail are visible at once -- two "target mass reached"
    // blocks (both fixture rows share that termination).
    expect(screen.getAllByText(/target mass reached/i)).toHaveLength(2);
  });

  it("moves focus between adjacent cells with arrow keys", () => {
    renderHeatMap();
    const start = screen.getByRole("button", { name: /temperature_profile_c\.constant 88, puck\.particle_diameter_um 250/i });
    start.focus();
    fireEvent.keyDown(start, { key: "ArrowRight" });
    const right = screen.getByRole("button", { name: /temperature_profile_c\.constant 88, puck\.particle_diameter_um 300/i });
    expect(right).toHaveFocus();
  });

  it("carries the metric value and unit in every cell's aria-label as a non-color alternative", () => {
    renderHeatMap();
    const cell = screen.getByRole("button", { name: /temperature_profile_c\.constant 88, puck\.particle_diameter_um 250/i });
    expect(cell.getAttribute("aria-label")).toMatch(/shot time \d+\.\d s/);
  });
});

describe("HeatMap: baseline coordinate (audit #05)", () => {
  it("marks the aria-label of the cell matching the baseline recipe's values", () => {
    renderHeatMap({ baselineY: 92, baselineX: 300 });
    const baselineCell = screen.getByRole("button", { name: /temperature_profile_c\.constant 92, puck\.particle_diameter_um 300/i });
    expect(baselineCell.getAttribute("aria-label")).toMatch(/baseline/i);

    const otherCell = screen.getByRole("button", { name: /temperature_profile_c\.constant 88, puck\.particle_diameter_um 250/i });
    expect(otherCell.getAttribute("aria-label")).not.toMatch(/baseline/i);
  });

  it("marks no cell as baseline when the coordinates are not given", () => {
    renderHeatMap();
    const group = screen.getByRole("group", { name: /heat map of/i });
    for (const button of within(group).getAllByRole("button")) {
      expect(button.getAttribute("aria-label")).not.toMatch(/baseline/i);
    }
  });
});

describe("HeatMap: large grids", () => {
  it("still renders one cell per coordinate at a 30x30 grid without crashing", () => {
    const xValues = Array.from({ length: 30 }, (_, i) => 200 + i * 10);
    const yValues = Array.from({ length: 30 }, (_, i) => 85 + i);
    const rows = [];
    let index = 0;
    for (const y of yValues) {
      for (const x of xValues) {
        rows.push(makeSweepRow({ index: index++, coordinates: [y, x], shot_time_s: 25 }));
      }
    }
    renderHeatMap({ rows, xValues, yValues });
    const group = screen.getByRole("group", { name: /heat map of/i });
    expect(within(group).getAllByRole("button")).toHaveLength(900);
  });
});
