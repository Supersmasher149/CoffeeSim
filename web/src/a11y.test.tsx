import { http, HttpResponse } from "msw";
import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, it } from "vitest";

import { App } from "./App";
import { server } from "./test/fixtures/server";
import { expectNoSeriousViolations } from "./test/a11y";
import { makeShotResult } from "./test/fixtures/shotResult";
import { makeCompletedSweep, makeSweepAccepted } from "./test/fixtures/sweep";
import { makeMeasuredShotComparison } from "./test/fixtures/measuredShot";

// Section 4 of the frontend test plan: axe against a handful of stable,
// representative states, gated on serious/critical violations only (see
// src/test/a11y.ts). This is not exhaustive per-component coverage --
// individual component test files assert the specific fixes (label
// association, button names, progress semantics, heat-map cell names) this
// gate would otherwise only report as an unlabelled violation.

describe("Accessibility: stable application states", () => {
  it("the empty application (no run yet) has no serious/critical violations", async () => {
    const { container } = render(<App />);
    await screen.findByRole("option", { name: /baseline/i });
    await expectNoSeriousViolations(container);
  });

  it("a completed simulation has no serious/critical violations", async () => {
    server.use(http.post("/api/v1/shots", () => HttpResponse.json(makeShotResult())));
    const user = userEvent.setup();
    const { container } = render(<App />);
    await screen.findByRole("option", { name: /baseline/i });
    await user.click(screen.getByRole("button", { name: /run simulation/i }));
    await screen.findByRole("link", { name: /download csv/i });
    await expectNoSeriousViolations(container);
  }, 15000);

  it("a completed measured-shot comparison has no serious/critical violations", async () => {
    server.use(
      http.get("/api/v1/measured-shots/:id/compare", () => HttpResponse.json(makeMeasuredShotComparison())),
    );
    const user = userEvent.setup();
    const { container } = render(<App />);
    await screen.findByRole("option", { name: /baseline/i });
    await user.click(screen.getByRole("tab", { name: /calibration/i }));
    // Audit #06: the measured-shot picker is folded into the shared
    // ground-truth list now, so the compare button is reachable directly
    // rather than through the comparison's own (now hidden) <select>.
    await user.click(await screen.findByRole("button", { name: /compare with default-v1/i }));
    await screen.findByText(/mass rmse/i);
    await expectNoSeriousViolations(container);
  }, 15000);

  it("a completed two-axis sweep (heat map) has no serious/critical violations", async () => {
    const twoAxisResult = makeCompletedSweep({
      axes: [
        { parameter_path: "temperature_profile_c.constant", values: [88, 92, 96] },
        { parameter_path: "puck.particle_diameter_um", values: [250, 300, 350] },
      ],
    });
    server.use(
      http.post("/api/v1/sweeps", () => HttpResponse.json(makeSweepAccepted({ total: 9 }))),
      http.get("/api/v1/sweeps/:id", () => HttpResponse.json(twoAxisResult)),
    );
    const user = userEvent.setup();
    const { container } = render(<App />);
    await screen.findByRole("option", { name: /baseline/i });
    await user.click(screen.getByRole("tab", { name: /sweeps/i }));
    await user.click(await screen.findByLabelText(/second axis \(heat map\)/i));
    await user.click(screen.getByRole("button", { name: /run sweep/i }));
    await screen.findByRole("group", { name: /heat map of/i });
    await expectNoSeriousViolations(container);
  }, 15000);

  it("an open diagnostics drawer and the always-visible profile editor have no serious/critical violations", async () => {
    server.use(http.post("/api/v1/shots", () => HttpResponse.json(makeShotResult())));
    const user = userEvent.setup();
    const { container } = render(<App />);
    await screen.findByRole("option", { name: /baseline/i });
    await user.click(screen.getByRole("button", { name: /run simulation/i }));
    await screen.findByRole("link", { name: /download csv/i });

    // <summary> is not universally exposed with an implicit "button" role by
    // the accessibility-tree approximation RTL's getByRole uses, so it is
    // targeted by its own text instead.
    await user.click(screen.getByText(/^Diagnostics —/));
    // Audit #08: the profile editor's numeric points are on screen by
    // default now, with no disclosure toggle to open first.
    await expectNoSeriousViolations(container);
  }, 15000);
});
