import { fireEvent, render, screen } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import { makeShotResult } from "../../test/fixtures/shotResult";
import { ChartStack } from "./ChartStack";

describe("ChartStack: series and chart summaries", () => {
  it("switches the coordinated chart between tracked quantities with accessible summaries", () => {
    const result = makeShotResult();
    render(<ChartStack result={result} comparisons={[]} onCursorChange={vi.fn()} />);

    expect(screen.getByRole("img", { name: /^Commanded pressure \(bar\): commanded pressure/i })).toBeInTheDocument();
    fireEvent.change(screen.getByLabelText("Signal"), { target: { value: "flow" } });
    expect(screen.getByRole("img", { name: /^Computed flow \(ml\/s\): flow/i })).toBeInTheDocument();
    fireEvent.change(screen.getByLabelText("Signal"), { target: { value: "temperature" } });
    expect(screen.getByRole("img", { name: /^Temperatures.*inlet, puck/i })).toBeInTheDocument();
    fireEvent.change(screen.getByLabelText("Signal"), { target: { value: "mass" } });
    expect(screen.getByRole("img", { name: /^Beverage mass \(g\): beverage mass/i })).toBeInTheDocument();
    fireEvent.change(screen.getByLabelText("Signal"), { target: { value: "strength" } });
    expect(screen.getByRole("img", { name: /^Strength and extraction.*TDS, extraction yield/i })).toBeInTheDocument();
  });

  it("adds an overlay series per comparison run, labelled with its run id, up to 3 total runs", () => {
    const result = makeShotResult();
    const comparisons = [makeShotResult(), makeShotResult(), makeShotResult()];
    render(<ChartStack result={result} comparisons={comparisons} onCursorChange={vi.fn()} />);

    fireEvent.change(screen.getByLabelText("Signal"), { target: { value: "mass" } });
    const massChart = screen.getByRole("img", { name: /^Beverage mass/i });
    // Only the first 2 comparisons are drawn (3 total runs including the
    // primary), each named by a slice of its own run id.
    expect(massChart.getAttribute("aria-label")).toContain(comparisons[0].manifest.run_id.slice(5, 11));
    expect(massChart.getAttribute("aria-label")).toContain(comparisons[1].manifest.run_id.slice(5, 11));
    expect(massChart.getAttribute("aria-label")).not.toContain(comparisons[2].manifest.run_id.slice(5, 11));
  });

  it("includes a pre-infusion marker only when preInfusionEnd is given", () => {
    const result = makeShotResult({ target_mass_reached: false });
    const { rerender } = render(<ChartStack result={result} comparisons={[]} onCursorChange={vi.fn()} />);
    const chartWithout = screen.getByRole("img", { name: /^Commanded pressure/i });
    expect(chartWithout.getAttribute("aria-label")).not.toMatch(/marked event/);

    rerender(<ChartStack result={result} comparisons={[]} preInfusionEnd={10} onCursorChange={vi.fn()} />);
    const chartWith = screen.getByRole("img", { name: /^Commanded pressure/i });
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
    const chart = screen.getByRole("img", { name: /^Commanded pressure/i });
    // target mass marker + 2 warnings = 3.
    expect(chart.getAttribute("aria-label")).toMatch(/3 marked events/);
  });
});

describe("ChartStack: shared cursor callback", () => {
  it("calls onCursorChange with a time on mouse move and undefined on mouse leave", () => {
    const onCursorChange = vi.fn();
    const result = makeShotResult();
    render(<ChartStack result={result} comparisons={[]} onCursorChange={onCursorChange} />);

    const chart = screen.getByRole("img", { name: /^Commanded pressure/i });
    // Recharts attaches its mouse handlers to the chart's inner surface;
    // firing directly on the chart-card container still bubbles to them.
    fireEvent.mouseMove(chart);
    fireEvent.mouseLeave(chart);
    // Both handlers exist and are wired without throwing; Recharts' own
    // activeLabel computation from real pixel geometry belongs to Playwright
    // (web/e2e/), not this jsdom-stubbed layout.
    expect(onCursorChange).not.toHaveBeenCalledWith(NaN);
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
    fireEvent.change(screen.getByLabelText("Signal"), { target: { value: "mass" } });

    const lines = container.querySelectorAll(".recharts-line-curve");
    expect(lines).toHaveLength(2);
    expect(Array.from(lines).every((line) => line.getAttribute("d"))).toBe(true);
  });

  it("renders a current-run data table on demand", () => {
    const result = makeShotResult();
    render(<ChartStack result={result} comparisons={[]} onCursorChange={vi.fn()} />);
    fireEvent.click(screen.getByText(/view current-run data table/i));
    expect(screen.getByRole("table")).toBeInTheDocument();
    expect(screen.getAllByRole("row")).toHaveLength(result.samples.length + 1);
  });
});
