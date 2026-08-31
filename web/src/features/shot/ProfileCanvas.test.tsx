import { fireEvent, render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";

import type { ProfilePoint } from "../../api/types";
import { ProfileCanvas } from "./ProfileCanvas";

const POINTS: ProfilePoint[] = [
  [0, 2],
  [10, 2],
  [20, 9],
];

function renderCanvas(points: ProfilePoint[] = POINTS, maxTimeSeconds = 30) {
  const onChange = vi.fn();
  render(
    <ProfileCanvas points={points} range={[0, 12]} maxTimeSeconds={maxTimeSeconds} unit="bar" color="#d98b4a" onChange={onChange} />,
  );
  return { onChange };
}

describe("ProfileCanvas: accessible slider state", () => {
  it("exposes every point as a slider with time/value in its accessible name and range attributes", () => {
    renderCanvas();
    const first = screen.getByRole("slider", { name: /profile point 1/i });
    expect(first).toHaveAttribute("aria-valuemin", "0");
    expect(first).toHaveAttribute("aria-valuemax", "12");
    expect(first).toHaveAttribute("aria-valuenow", "2");
    expect(first).toHaveAttribute("aria-valuetext", "0 seconds, 2 bar");
    expect(screen.getAllByRole("slider")).toHaveLength(3);
  });
});

describe("ProfileCanvas: keyboard movement", () => {
  it("moves a focused point's value up and down with ArrowUp/ArrowDown, snapped to 0.1", async () => {
    const user = userEvent.setup();
    const { onChange } = renderCanvas();
    const point = screen.getByRole("slider", { name: /profile point 1/i });
    point.focus();

    await user.keyboard("{ArrowUp}");
    expect(onChange).toHaveBeenLastCalledWith([[0, 2.1], [10, 2], [20, 9]]);

    await user.keyboard("{ArrowDown}");
    expect(onChange).toHaveBeenLastCalledWith([[0, 1.9], [10, 2], [20, 9]]);
  });

  it("moves a focused point's time left and right with ArrowLeft/ArrowRight, snapped to 0.5s", async () => {
    // ProfileCanvas is controlled entirely by its `points` prop and holds no
    // internal state, so -- as with ControlRail's number fields -- these
    // two keypresses each compute their patch from the same unchanged
    // `points` prop rather than accumulating on top of each other, the way
    // they would once App re-renders ProfileCanvas with the patched array.
    const user = userEvent.setup();
    const { onChange } = renderCanvas();
    const point = screen.getByRole("slider", { name: /profile point 2/i });
    point.focus();

    await user.keyboard("{ArrowRight}");
    expect(onChange).toHaveBeenLastCalledWith([[0, 2], [10.5, 2], [20, 9]]);

    await user.keyboard("{ArrowLeft}");
    expect(onChange).toHaveBeenLastCalledWith([[0, 2], [9.5, 2], [20, 9]]);
  });

  it("fences a moved point's time between its neighbours, never letting it cross them", async () => {
    const user = userEvent.setup();
    const { onChange } = renderCanvas([
      [0, 2],
      [1, 4],
      [20, 9],
    ]);
    const middle = screen.getByRole("slider", { name: /profile point 2/i });
    middle.focus();
    // Left neighbour is at t=0, so the fence is 0 + MIN_GAP_S (0.5); moving
    // left by 0.5 repeatedly must never push this point past that floor.
    await user.keyboard("{ArrowLeft}{ArrowLeft}{ArrowLeft}{ArrowLeft}");
    const lastCall = onChange.mock.calls.at(-1)![0] as ProfilePoint[];
    expect(lastCall[1][0]).toBeGreaterThanOrEqual(0.5);
  });

  it("fences the value within [low, high] on ArrowUp/ArrowDown at the range boundary", async () => {
    const user = userEvent.setup();
    const { onChange } = renderCanvas([[0, 12]]);
    const point = screen.getByRole("slider", { name: /profile point 1/i });
    point.focus();
    await user.keyboard("{ArrowUp}");
    expect(onChange).toHaveBeenLastCalledWith([[0, 12]]); // clamped at the range's high end
  });
});

describe("ProfileCanvas: pointer add/drag (identity SVG coordinate mapping from setup.ts)", () => {
  it("double-clicking empty plot area adds a new point, kept in time order", () => {
    const { onChange } = renderCanvas();
    const svg = screen.getByRole("group", { name: /bar profile editor/i });
    // toX(5) = PAD.left(30) + (5/30 span)*PLOT_WIDTH(282) ~= 77: well clear
    // of the MIN_GAP_S fence around the existing points at t=0, 10 and 20.
    fireEvent.doubleClick(svg, { clientX: 77, clientY: 60 });
    expect(onChange).toHaveBeenCalledTimes(1);
    const next = onChange.mock.calls[0][0] as ProfilePoint[];
    expect(next).toHaveLength(4);
    // Must stay sorted by time.
    const times = next.map((p) => p[0]);
    expect([...times].sort((a, b) => a - b)).toEqual(times);
  });

  it("refuses to add a point within MIN_GAP_S of an existing one", () => {
    const { onChange } = renderCanvas();
    const svg = screen.getByRole("group", { name: /bar profile editor/i });
    // toX(0) = PAD.left = 30, an existing point's exact time.
    fireEvent.doubleClick(svg, { clientX: 30, clientY: 60 });
    expect(onChange).not.toHaveBeenCalled();
  });

  it("dragging a point via pointer events updates its time and value", () => {
    const { onChange } = renderCanvas();
    const point = screen.getByRole("slider", { name: /profile point 1/i });
    const svg = screen.getByRole("group", { name: /bar profile editor/i });

    fireEvent.pointerDown(point, { pointerId: 1, clientX: 30, clientY: 10 });
    fireEvent.pointerMove(svg, { pointerId: 1, clientX: 60, clientY: 20 });
    expect(onChange).toHaveBeenCalled();
    const dragged = onChange.mock.calls.at(-1)![0] as ProfilePoint[];
    // toX/toY are identity-mapped in tests, so x=60 local -> some positive
    // time greater than 0, fenced below the next point's time.
    expect(dragged[0][0]).toBeGreaterThan(0);
    expect(dragged[0][0]).toBeLessThan(dragged[1][0]);

    fireEvent.pointerUp(svg, { pointerId: 1 });
  });
});
