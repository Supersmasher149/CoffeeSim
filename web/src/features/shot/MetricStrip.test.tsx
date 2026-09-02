import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";

import { makeShotResult } from "../../test/fixtures/shotResult";
import { MetricStrip } from "./MetricStrip";

describe("MetricStrip", () => {
  it("formats every metric to its documented precision and unit", () => {
    const result = makeShotResult({
      elapsed_time_s: 27.96,
      beverage_mass_g: 36.04,
      average_flow_ml_s: 1.926,
      tds_percent: 9.803,
      extraction_yield_percent: 20.97,
      brew_ratio: 2.0022,
      termination: "target_mass_reached",
    });
    render(<MetricStrip result={result} />);

    expect(screen.getByText("28.0")).toBeInTheDocument(); // shot time, 1 dp
    expect(screen.getByText("36.0")).toBeInTheDocument(); // beverage, 1 dp
    expect(screen.getByText("1.93")).toBeInTheDocument(); // avg flow, 2 dp
    expect(screen.getByText("9.80")).toBeInTheDocument(); // TDS, 2 dp
    expect(screen.getByText("20.97")).toBeInTheDocument(); // extraction, 2 dp
    expect(screen.getByText("1:2.00")).toBeInTheDocument(); // brew ratio
    expect(screen.getByText("target mass reached")).toBeInTheDocument(); // underscores -> spaces
  });

  it("sets the stop reason in body type, not the numeric display face", () => {
    // The strip's .value face is 27px Cormorant, sized for figures. Left in
    // that face, "target mass reached" wrapped onto three lines and set the
    // height of every other tile in the row; .metric-prose opts the one
    // non-numeric tile out of it.
    const result = makeShotResult({ termination: "target_mass_reached" });
    const { container } = render(<MetricStrip result={result} />);

    const prose = container.querySelectorAll(".metric-prose");
    expect(prose).toHaveLength(1);
    expect(prose[0]).toHaveTextContent("target mass reached");
    // Every figure tile keeps the display face.
    expect(container.querySelectorAll(".metric")).toHaveLength(7);
  });

  it("replaces every underscore in the termination reason, not just the first", () => {
    const result = makeShotResult({ termination: "maximum_time_reached_safely" });
    render(<MetricStrip result={result} />);
    expect(screen.getByText("maximum time reached safely")).toBeInTheDocument();
  });
});
