import { render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";

import { makeShotResult } from "../../test/fixtures/shotResult";
import { DiagnosticsDrawer } from "./DiagnosticsDrawer";

describe("DiagnosticsDrawer", () => {
  it("summarises zero warnings and zero clamps in the disclosure summary", () => {
    render(<DiagnosticsDrawer result={makeShotResult({ warnings: [], diagnostics: { ...makeShotResult().diagnostics, clamp_count: 0 } })} />);
    expect(screen.getByText(/0 warnings, 0 clamps/i)).toBeInTheDocument();
    expect(screen.getByText(/no warnings: nothing was clamped/i)).toBeInTheDocument();
  });

  it("pluralises singular counts correctly", () => {
    render(
      <DiagnosticsDrawer
        result={makeShotResult({
          warnings: [{ code: "HARD_STOP", message: "puck choked", time_s: 12.34, severity: "hard" }],
          diagnostics: { ...makeShotResult().diagnostics, clamp_count: 1 },
        })}
      />,
    );
    expect(screen.getByText(/1 warning, 1 clamp$/i)).toBeInTheDocument();
  });

  it("renders every warning with its code, time and severity class", () => {
    const result = makeShotResult({
      warnings: [
        { code: "LOW_FLOW", message: "flow near zero", time_s: 3, severity: "soft" },
        { code: "HARD_STOP", message: "puck choked", time_s: 12.345, severity: "hard" },
      ],
    });
    render(<DiagnosticsDrawer result={result} />);
    expect(screen.getByText("LOW_FLOW")).toBeInTheDocument();
    expect(screen.getByText(/at 3\.00 s/)).toBeInTheDocument();
    expect(screen.getByText("flow near zero")).toBeInTheDocument();
    expect(screen.getByText(/at 12\.35 s/)).toBeInTheDocument();
    // eslint-disable-next-line testing-library/no-node-access
    const hardWarning = screen.getByText("puck choked").closest(".warning");
    expect(hardWarning).toHaveClass("hard");
  });

  it("shows mass-balance residuals in exponential notation and the puck temperature span", () => {
    const result = makeShotResult({
      diagnostics: {
        water_mass_residual_g: 1.2e-9,
        solids_mass_residual_g: -3.4e-10,
        clamp_count: 0,
        step_count: 2100,
        min_permeability_m2: 5.6e-13,
        max_flow_ml_s: 2.4,
        min_puck_temperature_c: 21.4,
        max_puck_temperature_c: 92.1,
      },
    });
    render(<DiagnosticsDrawer result={result} />);
    expect(screen.getByText(/1\.20e\+?-?9\s*g|1\.20e-9 g/i)).toBeInTheDocument();
    expect(screen.getByText("2100")).toBeInTheDocument();
    expect(screen.getByText(/21\.4 to 92\.1 °C/)).toBeInTheDocument();
  });

  it("shows the manifest's reproducibility fields, truncating both hashes to 16 characters", () => {
    const result = makeShotResult({
      manifest: {
        ...makeShotResult().manifest,
        run_id: "run-0099",
        solver_version: "1.2.3-test",
        coefficient_id: "default",
        coefficient_version: "9",
        recipe_hash: "0123456789abcdef" + "f".repeat(48),
        result_hash: "fedcba9876543210" + "a".repeat(48),
        dt_s: 0.02,
        sample_interval_s: 0.5,
      },
    });
    render(<DiagnosticsDrawer result={result} />);
    expect(screen.getByText("run-0099")).toBeInTheDocument();
    expect(screen.getByText("1.2.3-test")).toBeInTheDocument();
    expect(screen.getByText("default v9")).toBeInTheDocument();
    expect(screen.getByText("0123456789abcdef…")).toBeInTheDocument();
    expect(screen.getByText("fedcba9876543210…")).toBeInTheDocument();
    expect(screen.getByText("0.02 s / 0.5 s")).toBeInTheDocument();
  });
});
