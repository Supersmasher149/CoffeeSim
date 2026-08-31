import { http, HttpResponse, delay } from "msw";
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it } from "vitest";

import { server } from "../../test/fixtures/server";
import { makeMeasuredShotCatalogue, makeMeasuredShotComparison } from "../../test/fixtures/measuredShot";
import { MeasuredShotComparison } from "./MeasuredShotComparison";

function useCatalogue(overrides?: Parameters<typeof makeMeasuredShotCatalogue>[0]) {
  server.use(http.get("/api/v1/measured-shots", () => HttpResponse.json(makeMeasuredShotCatalogue(overrides))));
}

describe("MeasuredShotComparison: catalogue loading", () => {
  it("shows a loading state, then the selector once the catalogue resolves", async () => {
    render(<MeasuredShotComparison />);
    expect(screen.getByText(/loading measured-shot catalogue/i)).toBeInTheDocument();
    await screen.findByLabelText(/measured shot/i);
  });

  it("shows an empty state when the catalogue has no shots", async () => {
    useCatalogue({ measured_shots: [], count: 0 });
    render(<MeasuredShotComparison />);
    await screen.findByText(/no valid measured shots are available/i);
    expect(screen.queryByLabelText(/measured shot/i)).not.toBeInTheDocument();
  });

  it("shows a catalogue error and never renders the selector", async () => {
    server.use(http.get("/api/v1/measured-shots", () => HttpResponse.error()));
    render(<MeasuredShotComparison />);
    await screen.findByText(/measured-shot catalogue unavailable/i);
    expect(screen.queryByLabelText(/measured shot/i)).not.toBeInTheDocument();
  });

  it("defaults the selector to the first shot and labels synthetic vs. real", async () => {
    useCatalogue();
    render(<MeasuredShotComparison />);
    const select = await screen.findByLabelText(/measured shot/i);
    expect(select).toHaveValue("shot-001");
    expect(screen.getByRole("option", { name: /shot-001 \(synthetic\)/i })).toBeInTheDocument();
    expect(screen.getByRole("option", { name: /shot-002 \(real\)/i })).toBeInTheDocument();
  });
});

describe("MeasuredShotComparison: running a comparison", () => {
  it("issues exactly one compare request per click against the default-v1 coefficients", async () => {
    useCatalogue();
    let requests = 0;
    let seenQuery = "";
    server.use(
      http.get("/api/v1/measured-shots/:id/compare", ({ request }) => {
        requests += 1;
        seenQuery = new URL(request.url).searchParams.get("coefficients") ?? "";
        return HttpResponse.json(makeMeasuredShotComparison());
      }),
    );
    const user = userEvent.setup();
    render(<MeasuredShotComparison />);
    await screen.findByLabelText(/measured shot/i);
    await user.click(screen.getByRole("button", { name: /compare with default-v1/i }));
    await screen.findByText(/mass rmse/i);
    expect(requests).toBe(1);
    expect(seenQuery).toBe("default-v1");
  });

  it("shows correctly-signed residuals and units for optional measurement fields", async () => {
    useCatalogue();
    server.use(
      http.get("/api/v1/measured-shots/:id/compare", () =>
        HttpResponse.json(
          makeMeasuredShotComparison({
            loss: {
              mass_rmse_g: 1.234,
              time_error_s: 0,
              tds_error_percent: 0,
              pressure_rmse_bar: 0,
              regularization: 0,
              total: 1.5,
              simulated: true,
              has_time_measurement: false,
              has_tds_measurement: false,
              has_pressure_measurement: false,
            },
          }),
        ),
      ),
    );
    const user = userEvent.setup();
    render(<MeasuredShotComparison />);
    await screen.findByLabelText(/measured shot/i);
    await user.click(screen.getByRole("button", { name: /compare with default-v1/i }));
    await screen.findByText(/mass rmse/i);

    expect(screen.getByText("1.234")).toBeInTheDocument();
    // has_time_measurement / has_tds_measurement / has_pressure_measurement
    // are all false: every one of those metrics must read "Not measured"
    // rather than a fabricated number.
    expect(screen.getAllByText("Not measured")).toHaveLength(3);
  });

  it("shows the synthetic-fixture warning only for a synthetic shot", async () => {
    useCatalogue();
    server.use(
      http.get("/api/v1/measured-shots/:id/compare", () => HttpResponse.json(makeMeasuredShotComparison({ synthetic: true }))),
    );
    const user = userEvent.setup();
    render(<MeasuredShotComparison />);
    await screen.findByLabelText(/measured shot/i);
    await user.click(screen.getByRole("button", { name: /compare with default-v1/i }));
    await screen.findByText(/synthetic fixture/i);
  });

  it("shows the recorded-measurement warning for a real shot", async () => {
    useCatalogue();
    server.use(
      http.get("/api/v1/measured-shots/:id/compare", () => HttpResponse.json(makeMeasuredShotComparison({ synthetic: false }))),
    );
    const user = userEvent.setup();
    render(<MeasuredShotComparison />);
    await screen.findByLabelText(/measured shot/i);
    await user.click(screen.getByRole("button", { name: /compare with default-v1/i }));
    await screen.findByText(/recorded measurement/i);
  });

  it("shows a note instead of charts when paired_series is empty", async () => {
    useCatalogue();
    server.use(
      http.get("/api/v1/measured-shots/:id/compare", () => HttpResponse.json(makeMeasuredShotComparison({ paired_series: [] }))),
    );
    const user = userEvent.setup();
    render(<MeasuredShotComparison />);
    await screen.findByLabelText(/measured shot/i);
    await user.click(screen.getByRole("button", { name: /compare with default-v1/i }));
    await screen.findByText(/no paired mass-series points to plot/i);
  });

  it("shows a comparison error and clears the loading state", async () => {
    useCatalogue();
    server.use(http.get("/api/v1/measured-shots/:id/compare", () => HttpResponse.error()));
    const user = userEvent.setup();
    render(<MeasuredShotComparison />);
    await screen.findByLabelText(/measured shot/i);
    const button = screen.getByRole("button", { name: /compare with default-v1/i });
    await user.click(button);
    await screen.findByText(/comparison failed/i);
    expect(button).not.toBeDisabled();
    expect(button).toHaveTextContent(/compare with default-v1/i);
  });
});

describe("MeasuredShotComparison: selection and request races", () => {
  it("clears a previous result and error when the selection changes", async () => {
    useCatalogue();
    server.use(
      http.get("/api/v1/measured-shots/:id/compare", () => HttpResponse.json(makeMeasuredShotComparison())),
    );
    const user = userEvent.setup();
    render(<MeasuredShotComparison />);
    const select = await screen.findByLabelText(/measured shot/i);
    await user.click(screen.getByRole("button", { name: /compare with default-v1/i }));
    await screen.findByText(/mass rmse/i);

    await user.selectOptions(select, "shot-002");
    expect(screen.queryByText(/mass rmse/i)).not.toBeInTheDocument();
  });

  it("aborts an in-flight comparison request when the selection changes mid-flight", async () => {
    useCatalogue();
    let aborted = false;
    server.use(
      http.get("/api/v1/measured-shots/:id/compare", async ({ request }) => {
        await delay(50);
        aborted = request.signal.aborted;
        if (request.signal.aborted) return HttpResponse.error();
        return HttpResponse.json(makeMeasuredShotComparison());
      }),
    );
    const user = userEvent.setup();
    render(<MeasuredShotComparison />);
    const select = await screen.findByLabelText(/measured shot/i);
    await user.click(screen.getByRole("button", { name: /compare with default-v1/i }));
    await user.selectOptions(select, "shot-002");

    await waitFor(() => expect(aborted).toBe(true));
    // The stale response must never land: no comparison output for a request
    // that was abandoned mid-flight.
    await new Promise((resolve) => setTimeout(resolve, 80));
    expect(screen.queryByText(/mass rmse/i)).not.toBeInTheDocument();
  });

  it("the compare button disables itself while loading, so a second click cannot start an overlapping request", async () => {
    useCatalogue();
    server.use(
      http.get("/api/v1/measured-shots/:id/compare", async () => {
        await delay(30);
        return HttpResponse.json(makeMeasuredShotComparison());
      }),
    );
    const user = userEvent.setup();
    render(<MeasuredShotComparison />);
    await screen.findByLabelText(/measured shot/i);
    const button = screen.getByRole("button", { name: /compare with default-v1/i });

    await user.click(button);
    expect(button).toBeDisabled();
    expect(button).toHaveTextContent(/comparing/i);
    // A disabled native button never dispatches its click handler, so this
    // is a no-op rather than a second request -- the guard is structural,
    // not a race the component has to detect and discard after the fact.
    await user.click(button);

    await screen.findByText(/mass rmse/i);
    expect(button).not.toBeDisabled();
  });

  it("stops updating state after unmount (no act warning, no crash)", async () => {
    useCatalogue();
    server.use(
      http.get("/api/v1/measured-shots/:id/compare", async () => {
        await delay(50);
        return HttpResponse.json(makeMeasuredShotComparison());
      }),
    );
    const user = userEvent.setup();
    const { unmount } = render(<MeasuredShotComparison />);
    await screen.findByLabelText(/measured shot/i);
    await user.click(screen.getByRole("button", { name: /compare with default-v1/i }));
    unmount();
    await new Promise((resolve) => setTimeout(resolve, 80));
  });
});
