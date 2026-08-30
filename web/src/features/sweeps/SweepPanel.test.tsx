import { delay, http, HttpResponse } from "msw";
import { render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";

import { server } from "../../test/fixtures/server";
import { makeRecipe } from "../../test/fixtures/recipe";
import { makeCompletedSweep, makeRunningSweep, makeSweepAccepted } from "../../test/fixtures/sweep";
import { SweepPanel } from "./SweepPanel";

const PARAMETERS = ["puck.particle_diameter_um", "temperature_profile_c.constant", "puck.dose_g"];

// SweepPanel polls every 250ms with a real setTimeout; rather than fighting
// userEvent + msw + fake timers for a savings of a few hundred milliseconds
// per test, these tests run on real timers and just wait for the terminal
// DOM state, with a generous waitFor timeout for the multi-poll cases.
function setup() {
  const onError = vi.fn();
  const user = userEvent.setup();
  const view = render(<SweepPanel baseline={makeRecipe()} parameters={PARAMETERS} onError={onError} />);
  return { user, onError, ...view };
}

describe("SweepPanel: starting a sweep", () => {
  it("posts a single-axis sweep using the primary axis's linspace", async () => {
    let body: { name: string; axes: { parameter_path: string; values: number[] }[] } | undefined;
    server.use(
      http.post("/api/v1/sweeps", async ({ request }) => {
        body = (await request.json()) as typeof body;
        return HttpResponse.json(makeSweepAccepted({ total: 9 }));
      }),
      http.get("/api/v1/sweeps/:id", () => HttpResponse.json(makeCompletedSweep())),
    );
    const { user } = setup();
    expect(screen.getByRole("button", { name: /run sweep \(9\)/i })).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: /run sweep/i }));
    await waitFor(() => expect(body).toBeDefined());
    expect(body!.name).toBe("dashboard");
    expect(body!.axes).toHaveLength(1);
    expect(body!.axes[0]).toEqual({
      parameter_path: "puck.particle_diameter_um",
      values: [250, 275, 300, 325, 350, 375, 400, 425, 450],
    });
  });

  it("posts a two-axis sweep with the secondary axis outer and the primary axis inner", async () => {
    let body: { axes: { parameter_path: string; values: number[] }[] } | undefined;
    server.use(
      http.post("/api/v1/sweeps", async ({ request }) => {
        body = (await request.json()) as typeof body;
        return HttpResponse.json(makeSweepAccepted({ total: 45 }));
      }),
      http.get("/api/v1/sweeps/:id", () => HttpResponse.json(makeCompletedSweep())),
    );
    const { user } = setup();
    await user.click(screen.getByLabelText(/second axis \(heat map\)/i));
    await user.click(screen.getByRole("button", { name: /run sweep/i }));
    await waitFor(() => expect(body).toBeDefined());
    expect(body!.axes).toHaveLength(2);
    expect(body!.axes[0].parameter_path).toBe("temperature_profile_c.constant");
    expect(body!.axes[1].parameter_path).toBe("puck.particle_diameter_um");
  });

  it("disables Run and warns once the run count exceeds 20000", async () => {
    const { user } = setup();
    await user.click(screen.getByLabelText(/second axis \(heat map\)/i));

    const steps = screen.getAllByLabelText(/^steps$/i);
    await user.clear(steps[0]);
    await user.type(steps[0], "600");
    await user.clear(steps[1]);
    await user.type(steps[1], "40");

    expect(screen.getByText(/limited to 20000 runs/i)).toBeInTheDocument();
    expect(screen.getByRole("button", { name: /run sweep/i })).toBeDisabled();
  });

  it("reports a startSweep failure through onError without starting to poll", async () => {
    server.use(
      http.post("/api/v1/sweeps", () =>
        HttpResponse.json({ error: { code: "INVALID_AXIS", message: "unknown parameter path", path: "axes[0]" } }, { status: 400 }),
      ),
    );
    const { user, onError } = setup();
    await user.click(screen.getByRole("button", { name: /run sweep/i }));
    await waitFor(() => expect(onError).toHaveBeenCalled());
  });

  it("locks submission before the acceptance response to prevent duplicate jobs", async () => {
    let starts = 0;
    server.use(
      http.post("/api/v1/sweeps", async () => {
        starts += 1;
        await delay(30);
        return HttpResponse.json(makeSweepAccepted({ total: 9 }));
      }),
      http.get("/api/v1/sweeps/:id", () => HttpResponse.json(makeCompletedSweep())),
    );
    const { user } = setup();
    const runButton = screen.getByRole("button", { name: /run sweep/i });
    await user.dblClick(runButton);
    expect(screen.getByRole("button", { name: /starting/i })).toBeDisabled();
    await waitFor(() => expect(starts).toBe(1));
  });
});

describe("SweepPanel: polling and terminal states", () => {
  it("polls repeatedly until the sweep completes, then shows the aggregate CSV link", async () => {
    let pollCount = 0;
    server.use(
      http.post("/api/v1/sweeps", () => HttpResponse.json(makeSweepAccepted({ total: 3 }))),
      http.get("/api/v1/sweeps/:id", () => {
        pollCount += 1;
        if (pollCount < 3) return HttpResponse.json(makeRunningSweep({ completed: pollCount, total: 3 }));
        return HttpResponse.json(makeCompletedSweep());
      }),
    );
    const { user } = setup();
    await user.click(screen.getByRole("button", { name: /run sweep/i }));

    await screen.findByRole("link", { name: /download aggregate csv/i }, { timeout: 3000 });
    expect(pollCount).toBeGreaterThanOrEqual(3);
    expect(screen.getByRole("button", { name: /run sweep/i })).not.toBeDisabled();
  });

  it("stops polling and reports the error through onError on a failed sweep", async () => {
    server.use(
      http.post("/api/v1/sweeps", () => HttpResponse.json(makeSweepAccepted({ total: 3 }))),
      http.get("/api/v1/sweeps/:id", () =>
        HttpResponse.json({ sweep_id: "sweep-0001", status: "failed", completed: 1, total: 3, elapsed_s: 0.4, error: { code: "SOLVER_DIVERGED", message: "puck choked" } }),
      ),
    );
    const { user, onError } = setup();
    await user.click(screen.getByRole("button", { name: /run sweep/i }));
    await waitFor(() => expect(onError).toHaveBeenCalledWith("SOLVER_DIVERGED: puck choked"));
    expect(screen.queryByRole("link", { name: /download aggregate csv/i })).not.toBeInTheDocument();
  });

  it("keeps a cancelled sweep's partial results and labels them as partial", async () => {
    const partial = makeCompletedSweep({
      status: "cancelled", cancelled: true, completed: 2, run_count: 2,
    });
    partial.runs = partial.runs!.slice(0, 2);
    server.use(
      http.post("/api/v1/sweeps", () => HttpResponse.json(makeSweepAccepted({ total: 9 }))),
      http.get("/api/v1/sweeps/:id", () => HttpResponse.json(partial)),
    );
    const { user } = setup();
    await user.click(screen.getByRole("button", { name: /run sweep/i }));
    await screen.findByText(/partial results kept/i);
    expect(screen.getAllByRole("row")).toHaveLength(1 + 2); // header + 2 kept runs
  });

  it("issues a cancel request against the running sweep id", async () => {
    let cancelledId = "";
    let statusCalls = 0;
    server.use(
      http.post("/api/v1/sweeps", () => HttpResponse.json(makeSweepAccepted({ total: 9 }))),
      http.get("/api/v1/sweeps/:id", () => {
        statusCalls += 1;
        // Stay "running" so the Cancel button remains available to click.
        return HttpResponse.json(makeRunningSweep({ completed: Math.min(statusCalls, 8), total: 9 }));
      }),
      http.post("/api/v1/sweeps/:id/cancel", ({ params }) => {
        cancelledId = params.id as string;
        return HttpResponse.json({ sweep_id: params.id, cancel_requested: true });
      }),
    );
    const { user } = setup();
    await user.click(screen.getByRole("button", { name: /run sweep/i }));
    const cancelButton = await screen.findByRole("button", { name: /^cancel$/i });
    await user.click(cancelButton);
    await waitFor(() => expect(cancelledId).toBe("sweep-0001"));
  });

  it("shows accessible progress semantics while a sweep is running", async () => {
    let statusCalls = 0;
    server.use(
      http.post("/api/v1/sweeps", () => HttpResponse.json(makeSweepAccepted({ total: 8 }))),
      http.get("/api/v1/sweeps/:id", () => {
        statusCalls += 1;
        return HttpResponse.json(makeRunningSweep({ completed: Math.min(statusCalls, 7), total: 8 }));
      }),
    );
    const { user } = setup();
    await user.click(screen.getByRole("button", { name: /run sweep/i }));
    const bar = await screen.findByRole("progressbar", { name: /sweep progress/i });
    expect(bar).toHaveAttribute("aria-valuemin", "0");
    expect(bar).toHaveAttribute("aria-valuemax", "8");
    expect(Number(bar.getAttribute("aria-valuenow"))).toBeGreaterThanOrEqual(1);
  });

  it("stops polling once the component unmounts", async () => {
    let pollCount = 0;
    server.use(
      http.post("/api/v1/sweeps", () => HttpResponse.json(makeSweepAccepted({ total: 9 }))),
      http.get("/api/v1/sweeps/:id", () => {
        pollCount += 1;
        return HttpResponse.json(makeRunningSweep({ completed: pollCount, total: 9 }));
      }),
    );
    const { user, unmount } = setup();
    await user.click(screen.getByRole("button", { name: /run sweep/i }));
    await waitFor(() => expect(pollCount).toBeGreaterThanOrEqual(1));

    unmount();
    const countAtUnmount = pollCount;
    await new Promise((resolve) => setTimeout(resolve, 600));
    // Real time passed well beyond two 250ms poll intervals; an un-cleaned-up
    // timer would have fired at least once more.
    expect(pollCount).toBe(countAtUnmount);
  });
});

describe("SweepPanel: completed-sweep display", () => {
  it("switches between the metric options and updates the chart summary", async () => {
    server.use(
      http.post("/api/v1/sweeps", () => HttpResponse.json(makeSweepAccepted({ total: 3 }))),
      http.get("/api/v1/sweeps/:id", () => HttpResponse.json(makeCompletedSweep())),
    );
    const { user } = setup();
    await user.click(screen.getByRole("button", { name: /run sweep/i }));
    // The default metric is shot time.
    await screen.findByRole("img", { name: /^shot time against/i });

    await user.selectOptions(screen.getByLabelText("Metric"), "extraction_yield_percent");
    await screen.findByRole("img", { name: /^extraction yield against/i });
  });

  it("renders a heat map with an accessible group summary for a completed two-axis sweep", async () => {
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
    const { user } = setup();
    await user.click(screen.getByLabelText(/second axis \(heat map\)/i));
    await user.click(screen.getByRole("button", { name: /run sweep/i }));
    const group = await screen.findByRole("group", { name: /heat map of/i });
    expect(within(group).getAllByRole("button").length).toBeGreaterThan(0);
  });

  it("lists every run in the results table with a stop-condition column", async () => {
    server.use(
      http.post("/api/v1/sweeps", () => HttpResponse.json(makeSweepAccepted({ total: 3 }))),
      http.get("/api/v1/sweeps/:id", () => HttpResponse.json(makeCompletedSweep())),
    );
    const { user } = setup();
    await user.click(screen.getByRole("button", { name: /run sweep/i }));
    const table = await screen.findByRole("table");
    expect(within(table).getAllByRole("row")).toHaveLength(1 + 3);
    expect(within(table).getAllByText(/target mass reached/i).length).toBe(3);
  });

  it("offers an aggregate CSV download scoped to the sweep id", async () => {
    server.use(
      http.post("/api/v1/sweeps", () => HttpResponse.json(makeSweepAccepted({ total: 3 }))),
      http.get("/api/v1/sweeps/:id", () => HttpResponse.json(makeCompletedSweep())),
    );
    const { user } = setup();
    await user.click(screen.getByRole("button", { name: /run sweep/i }));
    const link = await screen.findByRole("link", { name: /download aggregate csv/i });
    expect(link).toHaveAttribute("href", "/api/v1/artifacts/sweep-0001.csv");
  });
});
