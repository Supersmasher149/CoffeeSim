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

  it("replaces every underscore in the termination reason, not just the first", () => {
    const result = makeShotResult({ termination: "maximum_time_reached_safely" });
    render(<MetricStrip result={result} />);
    expect(screen.getByText("maximum time reached safely")).toBeInTheDocument();
  });
});
