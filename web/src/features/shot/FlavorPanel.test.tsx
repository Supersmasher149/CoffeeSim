import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";

import { SENSORY_AXES, SOLUTE_CLASSES } from "../../api/types";
import type { FlavorResult } from "../../api/types";
import { FlavorPanel } from "./FlavorPanel";

const FLAVOR: FlavorResult = {
  bean_id: "counter-culture-hologram",
  bean_version: "1.0.0",
  flavor_model_version: "flavor-0.1.0",
  match_score: 84.2,
  rms_deviation: 0.8,
  verdict: "balanced",
  dominant_deviation_axis: "fruit",
  class_clamp_count: 0,
  composition_residual_g: 1e-12,
  composition_percent: Object.fromEntries(
    SOLUTE_CLASSES.map((klass) => [klass, 16.6667]),
  ) as FlavorResult["composition_percent"],
  axes: Object.fromEntries(
    SENSORY_AXES.map((axis) => [axis, { intensity: 6.0, target: 6.0, deviation: 0.0 }]),
  ) as FlavorResult["axes"],
};

describe("FlavorPanel", () => {
  it("renders native flavour scores, targets, composition, and caveat", () => {
    render(<FlavorPanel flavor={FLAVOR} />);

    expect(screen.getByRole("heading", { name: /sensory estimate.*hologram/i })).toBeInTheDocument();
    expect(screen.getByText(/heuristic, uncalibrated/i)).toBeInTheDocument();
    expect(screen.getByText("84")).toBeInTheDocument();
    expect(screen.getByText(/balanced for this bean's target/i)).toBeInTheDocument();
    expect(screen.getByText(/furthest off: fruit/i)).toBeInTheDocument();
    expect(screen.getByText("Acids").parentElement).toHaveTextContent("16.7%");
    expect(screen.getByText(/authored solute-class priors/i)).toBeInTheDocument();
  });
});
