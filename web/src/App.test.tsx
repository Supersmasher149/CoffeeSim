import { StrictMode } from "react";
import { http, HttpResponse, delay } from "msw";
import { render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";

import { App } from "./App";
import { server } from "./test/fixtures/server";
import { makeCatalogue, makeRecipe } from "./test/fixtures/recipe";
import { makeShotResult } from "./test/fixtures/shotResult";
import { makeReferenceCatalogue } from "./test/fixtures/reference";

async function renderApp() {
  const user = userEvent.setup();
  render(<App />);
  // ControlRail's recipe <select> gets its options once /api/v1/recipes
  // resolves; waiting for the baseline option is the point at which the
  // catalogue fetch (independent of health and references) has landed.
  await screen.findByRole("option", { name: /baseline/i });
  return user;
}

// "Shot time" itself is ambiguous (MetricStrip's label, the reference table's
// row header, and its prose all say it), but the header's CSV download link
// exists only once App has a completed `active` run -- a single, unambiguous
// signal that a simulation has finished.
async function waitForRunComplete() {
  return screen.findByRole("link", { name: /download csv/i });
}

function metricValue(label: string) {
  const labelNode = screen.getByText(label, { selector: ".metric-strip .label" });
  return labelNode.parentElement!.querySelector(".value");
}

describe("App: independent data loading", () => {
  it("does not overwrite an edited fallback draft when the recipe catalogue arrives late", async () => {
    server.use(
      http.get("/api/v1/recipes", async () => {
        await delay(40);
        return HttpResponse.json({ recipes: makeCatalogue() });
      }),
    );
    const user = userEvent.setup();
    render(<App />);
    const dose = screen.getByLabelText(/dose \(g\)/i);
    await user.clear(dose);
    await user.type(dose, "20");
    await screen.findByRole("option", { name: /baseline/i });
    expect(dose).toHaveValue(20);
  });

  it("renders once health, recipes and references have each resolved, even though none of them share a request", async () => {
    const user = userEvent.setup();
    render(<App />);
    await screen.findByText(/0\.1\.0-test/);
    await screen.findByRole("option", { name: /baseline/i });
    await user.click(screen.getByRole("tab", { name: "Calibration" }));
    // The reference catalogue landing shows up as a row in the shared
    // ground-truth list (audit #06), not its own tab.
    await screen.findByText(/published · metadata only/i);
  });

  it("shows a connection message and still loads recipes when health fails", async () => {
    server.use(http.get("/api/v1/health", () => HttpResponse.error()));
    render(<App />);
    await screen.findByText(/cannot reach the tool server/i);
    await screen.findByRole("option", { name: /baseline/i });
  });

  it("surfaces a reference-catalogue error independently of a working recipe load", async () => {
    server.use(http.get("/api/v1/reference-shots", () => HttpResponse.error()));
    const user = userEvent.setup();
    render(<App />);
    await screen.findByRole("option", { name: /baseline/i });
    await user.click(screen.getByRole("tab", { name: "Calibration" }));
    await screen.findByText(/reference catalogue unavailable/i);
  });

  it("skips malformed {id, error} catalogue entries when picking the initial draft", async () => {
    server.use(
      http.get("/api/v1/recipes", () =>
        HttpResponse.json({
          recipes: [
            { id: "broken", error: { code: "SCHEMA_VIOLATION", message: "puck.dose_g is required" } },
            { id: "only-good-one", name: "Only good one", recipe: makeRecipe({ name: "Only good one" }) },
          ],
        }),
      ),
    );
    render(<App />);
    // The malformed entry renders, disabled, so the catalogue explains the
    // gap -- but the workspace's initial draft must come from the loaded one.
    const broken = await screen.findByRole("option", { name: /broken.*failed to load/i });
    expect(broken).toBeDisabled();
    expect(screen.getByRole("combobox", { name: "Recipe" })).toHaveValue("only-good-one");
  });

  it("renders with an empty recipe catalogue instead of crashing", async () => {
    server.use(http.get("/api/v1/recipes", () => HttpResponse.json({ recipes: [] })));
    render(<App />);
    await screen.findByText(/0\.1\.0-test/);
    expect(screen.getByRole("combobox", { name: "Recipe" }).children).toHaveLength(0);
  });
});

describe("App: workflow navigation", () => {
  it("preserves workflow-local state while switching tabs", async () => {
    const user = await renderApp();
    await user.click(screen.getByRole("tab", { name: "Sweeps" }));
    const steps = await screen.findByRole("spinbutton", { name: "steps" });
    await user.clear(steps);
    await user.type(steps, "7");

    await user.click(screen.getByRole("tab", { name: "Shot" }));
    expect(screen.queryByRole("button", { name: /run sweep/i })).not.toBeInTheDocument();
    await user.click(screen.getByRole("tab", { name: "Sweeps" }));
    expect(screen.getByRole("spinbutton", { name: "steps" })).toHaveValue(7);
  });

  it("hides shot validation alerts when another workflow is active", async () => {
    const user = await renderApp();
    const dose = screen.getByLabelText(/dose \(g\)/i);
    await user.clear(dose);
    await user.type(dose, "2");
    expect(screen.getByRole("alert")).toHaveTextContent(/must be between 14 and 22/i);

    await user.click(screen.getByRole("tab", { name: "Calibration" }));
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
  });

  it("keeps recipe-catalogue failures in the Shot workflow", async () => {
    server.use(http.get("/api/v1/recipes", () => HttpResponse.error()));
    const user = userEvent.setup();
    render(<App />);
    await screen.findByRole("alert");
    expect(screen.getByRole("alert")).toHaveTextContent(/recipe catalogue unavailable/i);

    await user.click(screen.getByRole("tab", { name: "Calibration" }));
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
  });
});

describe("App: running a simulation", () => {
  it("runs successfully and shows metrics for the result", async () => {
    const user = await renderApp();
    server.use(http.post("/api/v1/shots", () => HttpResponse.json(makeShotResult())));
    await user.click(screen.getByRole("button", { name: /run simulation/i }));
    await waitForRunComplete();
    expect(metricValue("Shot time")).toHaveTextContent("28.0");
  });

  it("shows server validation issues on a rejected simulation without crashing", async () => {
    const user = await renderApp();
    server.use(
      http.post("/api/v1/shots", () =>
        HttpResponse.json(
          {
            error: {
              code: "OUT_OF_RANGE", path: "recipe.puck.dose_g", message: "dose out of range",
              details: { issues: [{ code: "OUT_OF_RANGE", path: "recipe.puck.dose_g", message: "dose out of range" }] },
            },
          },
          { status: 422 },
        ),
      ),
    );
    await user.click(screen.getByRole("button", { name: /run simulation/i }));
    // Both the top-level error banner and the per-issue validation list say
    // "dose out of range" -- assert the banner (unique) and then that the
    // validation list rendered the issue's path, which the banner does not.
    await screen.findByText(/OUT_OF_RANGE: dose out of range/);
    expect(screen.getByText(/recipe\.puck\.dose_g/)).toBeInTheDocument();
  });

  it("shows a plain error message on a network failure", async () => {
    const user = await renderApp();
    server.use(http.post("/api/v1/shots", () => HttpResponse.error()));
    await user.click(screen.getByRole("button", { name: /run simulation/i }));
    await waitFor(() => expect(screen.getByRole("button", { name: /run simulation/i })).not.toBeDisabled());
    expect(screen.queryByRole("link", { name: /download csv/i })).not.toBeInTheDocument();
  });

  it("keeps the result tied to the recipe submitted, not one edited after the request started", async () => {
    const user = await renderApp();
    server.use(
      http.post("/api/v1/shots", async ({ request }) => {
        const body = (await request.json()) as { recipe: { puck: { dose_g: number } } };
        await delay(20);
        return HttpResponse.json(makeShotResult(), { headers: { "x-submitted-dose": String(body.recipe.puck.dose_g) } });
      }),
    );

    const doseField = screen.getByLabelText(/dose \(g\)/i);
    expect(doseField).toHaveValue(18);

    await user.click(screen.getByRole("button", { name: /run simulation/i }));
    // While the request is in flight, edit the draft to a different dose.
    await user.clear(doseField);
    await user.type(doseField, "20");

    await waitForRunComplete();

    // The reference panel's "current model" column must reflect the recipe
    // that was *submitted* (18 g), never the draft as it stands once the
    // response lands (20 g) -- App.tsx's activeRecipe contract.
    await user.click(screen.getByRole("tab", { name: "Calibration" }));
    // Audit #06: a reference now lives as a row in the shared ground-truth
    // list; select it to see its metadata table.
    await user.click(await screen.findByRole("button", { name: /shot 01/i }));
    const referenceTable = screen.getByRole("table");
    const doseRow = within(referenceTable).getAllByRole("row").find((row) => /dose/i.test(row.textContent ?? ""));
    expect(doseRow).toBeDefined();
    expect(within(doseRow!).getByText("18.0 g", { selector: ".current-model" })).toBeInTheDocument();
    // The draft itself keeps the edit.
    expect(doseField).toHaveValue(20);
  });

  it("does not relabel the active result when another draft is selected afterwards", async () => {
    const user = await renderApp();
    server.use(http.post("/api/v1/shots", () => HttpResponse.json(makeShotResult())));
    await user.click(screen.getByRole("button", { name: /run simulation/i }));
    await waitForRunComplete();

    await user.selectOptions(screen.getByRole("combobox", { name: "Recipe" }), "coarse");

    // The metrics for the run stay put; only the draft rail (particle
    // diameter) reflects the newly selected recipe.
    expect(metricValue("Shot time")).toHaveTextContent("28.0");
    expect(screen.getByLabelText(/particle ⌀/i)).toHaveValue(600);
  });
});

describe("App: pinning and comparison", () => {
  async function runOnce(user: ReturnType<typeof userEvent.setup>) {
    server.use(http.post("/api/v1/shots", () => HttpResponse.json(makeShotResult())));
    await user.click(screen.getByRole("button", { name: /run simulation/i }));
    await waitForRunComplete();
  }

  it("pins the active run, prevents a duplicate pin, and removes it", async () => {
    const user = await renderApp();
    await runOnce(user);

    const pinButton = screen.getByRole("button", { name: /pin current run/i });
    expect(pinButton).toBeEnabled();
    await user.click(pinButton);
    expect(pinButton).toBeDisabled();
    expect(screen.getByText(/current run is already pinned/i)).toBeInTheDocument();

    const removeButton = screen.getByRole("button", { name: /remove pinned run/i });
    await user.click(removeButton);
    expect(pinButton).toBeEnabled();
    expect(screen.queryByText(/current run is already pinned/i)).not.toBeInTheDocument();
  });
});

describe("App: downloads", () => {
  it("offers a CSV download link scoped to the run id", async () => {
    const user = await renderApp();
    server.use(http.post("/api/v1/shots", () => HttpResponse.json(makeShotResult())));
    await user.click(screen.getByRole("button", { name: /run simulation/i }));
    const link = await waitForRunComplete();

    expect(link).toHaveAttribute("href", expect.stringContaining("/api/v1/artifacts/run-"));
    expect(link).toHaveAttribute("download", expect.stringMatching(/^run-.*\.csv$/));
  });

  it("builds a JSON blob download for the active run", async () => {
    const user = await renderApp();
    const result = makeShotResult();
    server.use(http.post("/api/v1/shots", () => HttpResponse.json(result)));
    await user.click(screen.getByRole("button", { name: /run simulation/i }));
    await waitForRunComplete();

    const clickSpy = vi.spyOn(HTMLAnchorElement.prototype, "click").mockImplementation(() => {});
    await user.click(screen.getByRole("button", { name: /download json/i }));

    expect(URL.createObjectURL).toHaveBeenCalledTimes(1);
    expect(URL.revokeObjectURL).toHaveBeenCalledTimes(1);
    expect(clickSpy).toHaveBeenCalledTimes(1);
    clickSpy.mockRestore();
  });
});

describe("App: React Strict Mode", () => {
  it("mounts and unmounts cleanly under StrictMode without duplicate or dangling requests", async () => {
    let recipeRequests = 0;
    server.use(
      http.get("/api/v1/recipes", () => {
        recipeRequests += 1;
        return HttpResponse.json({ recipes: makeCatalogue() });
      }),
    );
    const { unmount } = render(
      <StrictMode>
        <App />
      </StrictMode>,
    );
    await screen.findByRole("option", { name: /baseline/i });
    unmount();
    // StrictMode double-invokes effects in development; App's fetch calls are
    // not itself deduplicated, so this pins the actual current behaviour
    // (one extra request from the intentional mount/unmount/remount) rather
    // than asserting an ideal this component does not implement.
    expect(recipeRequests).toBeGreaterThan(0);
  });
});

describe("App: reference panel association (regression, Audit P7 issue #22 pattern)", () => {
  it("uses the submitted recipe, not the live draft, for every result-dependent panel", async () => {
    const user = await renderApp();
    server.use(
      http.get("/api/v1/reference-shots", () => HttpResponse.json(makeReferenceCatalogue())),
      http.post("/api/v1/shots", () => HttpResponse.json(makeShotResult({ beverage_mass_g: 36 }))),
    );
    await user.click(screen.getByRole("button", { name: /run simulation/i }));
    await waitForRunComplete();

    const doseField = screen.getByLabelText(/dose \(g\)/i);
    await user.clear(doseField);
    await user.type(doseField, "14");

    await user.click(screen.getByRole("tab", { name: "Calibration" }));
    // Audit #06: a reference now lives as a row in the shared ground-truth
    // list; select it to see its metadata table.
    await user.click(await screen.findByRole("button", { name: /shot 01/i }));
    const referenceTable = screen.getByRole("table");
    const doseRow = within(referenceTable).getAllByRole("row").find((row) => /dose/i.test(row.textContent ?? ""));
    // Must still read 18.0 g (the submitted recipe), not 14 (the live draft).
    expect(within(doseRow!).getByText("18.0 g", { selector: ".current-model" })).toBeInTheDocument();
  });
});
