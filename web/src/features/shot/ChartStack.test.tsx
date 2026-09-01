import { fireEvent, render, screen } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import { makeShotResult } from "../../test/fixtures/shotResult";
import { ChartStack } from "./ChartStack";

describe("ChartStack: all channels drawn together", () => {
  it("draws pressure, flow, mass and temperature simultaneously with no signal selector", () => {
    const result = makeShotResult();
    render(<ChartStack result={result} comparisons={[]} onCursorChange={vi.fn()} />);

    // Audit #04: there is no more single-signal <select> gating the other
    // three channels off screen -- one well, one accessible summary, all
    // four channels named in it.
    expect(screen.queryByLabelText("Signal")).not.toBeInTheDocument();
    const chart = screen.getByRole("img", { name: /^Shot analysis: pressure, flow, mass, temperature/i });
    expect(chart).toBeInTheDocument();
    for (const key of ["pressure", "flow", "mass", "temperature"]) {
      expect(chart.querySelector(`path[data-series="${key}"]`)).toBeInTheDocument();
    }
  });

  it("adds an overlay series per comparison run, labelled with its run id, up to 3 total runs", () => {
    const result = makeShotResult();
    const comparisons = [makeShotResult(), makeShotResult(), makeShotResult()];
    render(<ChartStack result={result} comparisons={comparisons} onCursorChange={vi.fn()} />);

    const chart = screen.getByRole("img", { name: /^Shot analysis/i });
    // Only the first 2 comparisons are drawn (3 total runs including the
    // primary), each named by a slice of its own run id.
    expect(chart.getAttribute("aria-label")).toContain(comparisons[0].manifest.run_id.slice(5, 11));
    expect(chart.getAttribute("aria-label")).toContain(comparisons[1].manifest.run_id.slice(5, 11));
    expect(chart.getAttribute("aria-label")).not.toContain(comparisons[2].manifest.run_id.slice(5, 11));
    expect(chart.querySelector(`path[data-series="mass_${comparisons[0].manifest.run_id}"]`)).toBeInTheDocument();
  });

  it("keeps a pinned run's dash pattern stable when another pinned run is removed (audit #07)", () => {
    const result = makeShotResult();
    const first = makeShotResult();
    const second = makeShotResult();
    const { rerender } = render(
      <ChartStack result={result} comparisons={[first, second]} onCursorChange={vi.fn()} />,
    );
    const dashBefore = screen
      .getByRole("img", { name: /^Shot analysis/i })
      .querySelector(`path[data-series="pressure_${second.manifest.run_id}"]`)
      ?.getAttribute("stroke-dasharray");

    rerender(<ChartStack result={result} comparisons={[second]} onCursorChange={vi.fn()} />);
    const dashAfter = screen
      .getByRole("img", { name: /^Shot analysis/i })
      .querySelector(`path[data-series="pressure_${second.manifest.run_id}"]`)
      ?.getAttribute("stroke-dasharray");

    expect(dashAfter).toBe(dashBefore);
  });

  it("includes a pre-infusion marker only when preInfusionEnd is given", () => {
    const result = makeShotResult({ target_mass_reached: false });
    const { rerender } = render(<ChartStack result={result} comparisons={[]} onCursorChange={vi.fn()} />);
    const chartWithout = screen.getByRole("img", { name: /^Shot analysis/i });
    expect(chartWithout.getAttribute("aria-label")).not.toMatch(/marked event/);

    rerender(<ChartStack result={result} comparisons={[]} preInfusionEnd={10} onCursorChange={vi.fn()} />);
    const chartWith = screen.getByRole("img", { name: /^Shot analysis/i });
    expect(chartWith.getAttribute("aria-label")).toMatch(/1 marked event/);
  });

  it("adds one marker per warning on top of the target-mass marker", () => {
    const result = makeShotResult({
      target_mass_reached: true,
      warnings: [
        { code: "LOW_FLOW", message: "x", time_s: 4, severity: "soft" },
        { code: "HARD_STOP", message: "y", time_s: 8, severity: "hard" },
      ],
    });
    render(<ChartStack result={result} comparisons={[]} onCursorChange={vi.fn()} />);
    const chart = screen.getByRole("img", { name: /^Shot analysis/i });
    // target mass marker + 2 warnings = 3.
    expect(chart.getAttribute("aria-label")).toMatch(/3 marked events/);
  });
});

describe("ChartStack: shared cursor callback", () => {
  it("calls onCursorChange with a time on mouse move and undefined on mouse leave", () => {
    const onCursorChange = vi.fn();
    const result = makeShotResult();
    const { container } = render(
      <ChartStack result={result} comparisons={[]} onCursorChange={onCursorChange} />,
    );

    const svg = container.querySelector(".analysis-canvas-svg")!;
    fireEvent.mouseMove(svg, { clientX: 50 });
    fireEvent.mouseLeave(svg);
    expect(onCursorChange).toHaveBeenCalledWith(undefined);
    expect(onCursorChange).not.toHaveBeenCalledWith(NaN);
  });

  it("renders the cursor readout, with pinned-run values, once cursorTimeSeconds is fed back in", () => {
    // Regression test: ChartStack used to forward onCursorChange to
    // AnalysisCanvas but never accepted cursorTimeSeconds back from its
    // parent, so the readout box built by that value never rendered even
    // though the mouse-move callback fired correctly.
    const result = makeShotResult();
    const comparison = makeShotResult();
    const { container } = render(
      <ChartStack
        result={result}
        comparisons={[comparison]}
        cursorTimeSeconds={result.samples[1].time_s}
        onCursorChange={vi.fn()}
      />,
    );

    const readout = container.querySelector(".cursor-readout");
    expect(readout).toBeInTheDocument();
    expect(readout).toHaveTextContent(/t = /);
    // One row per primary channel plus one per overlay channel for the
    // pinned comparison run.
    expect(container.querySelectorAll(".cursor-readout-row").length).toBeGreaterThanOrEqual(8);
    expect(readout!.textContent).toContain(comparison.manifest.run_id.slice(5, 11));
  });
});

describe("ChartStack: differing sample intervals between overlay runs", () => {
  it("does not throw when a comparison run has fewer samples than the primary run", () => {
    const result = makeShotResult();
    const shortComparison = makeShotResult({ samples: makeShotResult().samples.slice(0, 2) });
    expect(() =>
      render(<ChartStack result={result} comparisons={[shortComparison]} onCursorChange={vi.fn()} />),
    ).not.toThrow();
  });

  it("connects a comparison series whose timestamps do not match the primary run", () => {
    const result = makeShotResult();
    const comparison = makeShotResult();
    comparison.samples = comparison.samples.map((sample) => ({
      ...sample,
      time_s: sample.time_s + 0.25,
    }));
    const { container } = render(
      <ChartStack result={result} comparisons={[comparison]} onCursorChange={vi.fn()} />,
    );

    const massPath = container.querySelector(`path[data-series="mass"]`);
    const overlayMassPath = container.querySelector(`path[data-series="mass_${comparison.manifest.run_id}"]`);
    expect(massPath?.getAttribute("d")).toBeTruthy();
    expect(overlayMassPath?.getAttribute("d")).toBeTruthy();
  });

  it("renders a current-run data table on demand", () => {
    const result = makeShotResult();
    render(<ChartStack result={result} comparisons={[]} onCursorChange={vi.fn()} />);
    fireEvent.click(screen.getByText(/view current-run data table/i));
    expect(screen.getByRole("table")).toBeInTheDocument();
    expect(screen.getAllByRole("row")).toHaveLength(result.samples.length + 1);
  });
});
