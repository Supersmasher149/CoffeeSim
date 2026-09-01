import { fireEvent, render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";

import type { ProfilePoint } from "../../api/types";
import { ProfileEditor } from "./ProfileEditor";

const POINTS: ProfilePoint[] = [
  [0, 2],
  [6, 2],
  [10, 9],
];

function renderEditor(points: ProfilePoint[] = POINTS) {
  const onChange = vi.fn();
  render(
    <ProfileEditor points={points} range={[0, 12]} maxTimeSeconds={30} unit="bar" color="#d98b4a" onChange={onChange} />,
  );
  return { onChange };
}

describe("ProfileEditor: numeric points are always visible (audit #08)", () => {
  it("shows the numeric point list on mount, with no disclosure toggle to find first", () => {
    renderEditor();
    expect(screen.queryByRole("button", { name: /edit numeric points/i })).not.toBeInTheDocument();
    expect(screen.queryByRole("button", { name: /hide numeric points/i })).not.toBeInTheDocument();
    expect(screen.getByLabelText(/point 1 time in seconds/i)).toHaveValue(0);
    expect(screen.getByLabelText(/point 2 value in bar/i)).toHaveValue(2);
  });
});

describe("ProfileEditor: numeric editing", () => {
  it("patches only the edited point's time, leaving the rest of the array untouched", () => {
    const { onChange } = renderEditor();

    fireEvent.change(screen.getByLabelText(/point 2 time in seconds/i), { target: { value: "7.5" } });
    expect(onChange).toHaveBeenCalledWith([
      [0, 2],
      [7.5, 2],
      [10, 9],
    ]);
  });

  it("patches only the edited point's value", () => {
    const { onChange } = renderEditor();

    fireEvent.change(screen.getByLabelText(/point 3 value in bar/i), { target: { value: "8" } });
    expect(onChange).toHaveBeenCalledWith([
      [0, 2],
      [6, 2],
      [10, 8],
    ]);
  });

  it("adds a point 5 seconds after the last one, at the last point's value", async () => {
    const user = userEvent.setup();
    const { onChange } = renderEditor();
    await user.click(screen.getByRole("button", { name: /add profile point/i }));
    expect(onChange).toHaveBeenCalledWith([...POINTS, [15, 9]]);
  });

  it("adds the first point at t=0 for an empty profile", async () => {
    const user = userEvent.setup();
    const { onChange } = renderEditor([]);
    await user.click(screen.getByRole("button", { name: /add profile point/i }));
    expect(onChange).toHaveBeenCalledWith([[0, 0]]);
  });

  it("removes the targeted point by index", async () => {
    const user = userEvent.setup();
    const { onChange } = renderEditor();
    await user.click(screen.getByRole("button", { name: /remove point 2/i }));
    expect(onChange).toHaveBeenCalledWith([
      [0, 2],
      [10, 9],
    ]);
  });

  it("disables removal once only one point remains", () => {
    renderEditor([[0, 6]]);
    expect(screen.getByRole("button", { name: /remove point 1/i })).toBeDisabled();
  });
});
