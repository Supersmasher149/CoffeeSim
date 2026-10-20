import { http, HttpResponse } from "msw";
import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it } from "vitest";

import { SENSORY_AXES, SOLUTE_CLASSES } from "../../api/types";
import type { FlavorResult, TastingReading, TastingReadingEntry } from "../../api/types";
import { server } from "../../test/fixtures/server";
import { makeTastingCatalogue, makeTastingReading } from "../../test/fixtures/tasting";
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

// Every overlay axis sits at 6.0, i.e. in the "pronounced" band.
function readingWith(overrides: Partial<TastingReading> = {}) {
  const base = makeTastingReading();
  return makeTastingReading({
    ...overrides,
    axes: {
      ...base.axes,
      bitterness: { level: "pronounced", status: "read", confidence: 0.9 },
      fruit: { level: "moderate", status: "read", confidence: 0.8 },
      body: { level: null, status: "low_confidence", confidence: 0.3 },
      ...overrides.axes,
    },
  });
}

function serve(...readings: TastingReadingEntry[]) {
  server.use(http.get("/api/v1/tasting-readings", () => HttpResponse.json(makeTastingCatalogue(readings))));
}

describe("FlavorPanel", () => {
  it("renders native flavour scores, targets, composition, and caveat", async () => {
    render(<FlavorPanel flavor={FLAVOR} />);

    expect(screen.getByRole("heading", { name: /sensory estimate.*hologram/i })).toBeInTheDocument();
    expect(screen.getByText(/heuristic, uncalibrated/i)).toBeInTheDocument();
    expect(screen.getByText("84")).toBeInTheDocument();
    expect(screen.getByText(/balanced for this bean's target/i)).toBeInTheDocument();
    expect(screen.getByText(/furthest off: fruit/i)).toBeInTheDocument();
    expect(screen.getByText("Acids").parentElement).toHaveTextContent("16.7%");
    expect(screen.getByText(/authored solute-class priors/i)).toBeInTheDocument();
    expect(await screen.findByText(/no tasting readings yet/i)).toBeInTheDocument();
  });
});

describe("FlavorPanel: tasting readings", () => {
  it("draws nothing extra until a reading is picked", async () => {
    serve(readingWith());
    render(<FlavorPanel flavor={FLAVOR} />);

    const select = await screen.findByLabelText(/compare with tasting notes/i);
    expect(select).toHaveValue("");
    expect(screen.getByRole("option", { name: "2026-10-20-test-01 · local model" })).toBeInTheDocument();
    expect(screen.queryByText(/taster:/i)).not.toBeInTheDocument();
    expect(document.querySelector(".flavor-tasted-band")).toBeNull();
  });

  it("shows each tasted level beside the estimate and counts in-band axes", async () => {
    serve(readingWith({ channeling_reported_probability: 0.93 }));
    render(<FlavorPanel flavor={FLAVOR} />);

    await userEvent.selectOptions(
      await screen.findByLabelText(/compare with tasting notes/i),
      "2026-10-20-test-01.tasting.local",
    );

    expect(screen.getByText("pronounced")).toBeInTheDocument();
    expect(screen.getByText("moderate")).toBeInTheDocument();
    expect(screen.getByText("unsure")).toBeInTheDocument();
    expect(screen.getAllByText("not mentioned")).toHaveLength(4);
    // Two axes have a level; only bitterness agrees with the 6.0 estimate.
    expect(screen.getByText(/inside the tasted band on 1 of 2 axes \(5 not mentioned or unsure\)/i)).toBeInTheDocument();
    expect(screen.getByText(/taster: sour \/ under-extracted/i)).toBeInTheDocument();
    expect(screen.getByText(/channeling reported/i)).toBeInTheDocument();
    expect(screen.getByText(/ai reading \(local model qwen2\.5:7b-instruct\)/i)).toBeInTheDocument();
    expect(document.querySelectorAll(".flavor-tasted-band")).toHaveLength(2);
  });

  it("labels a Jev reading as Jev and hides channeling when unlikely", async () => {
    serve(readingWith({ id: "jev-01.tasting.jev", backend: "typesafe", model: "jev-1.14.0" }));
    render(<FlavorPanel flavor={FLAVOR} />);

    await userEvent.selectOptions(await screen.findByLabelText(/compare with tasting notes/i), "jev-01.tasting.jev");
    expect(screen.getByRole("option", { name: /· jev$/i })).toBeInTheDocument();
    expect(screen.getByText(/ai reading \(jev-1\.14\.0\)/i)).toBeInTheDocument();
    expect(screen.queryByText(/channeling reported/i)).not.toBeInTheDocument();
  });

  it("reports unreadable reading files without hiding the valid ones", async () => {
    serve(readingWith(), { id: "broken.tasting.jev", error: { code: "TASTING_READING_INVALID", message: "bad" } });
    render(<FlavorPanel flavor={FLAVOR} />);

    expect(await screen.findByText(/1 reading file could not be read/i)).toBeInTheDocument();
    expect(screen.getAllByRole("option")).toHaveLength(2); // None + the valid reading
  });

  it("says so when the readings endpoint fails, and still shows the estimate", async () => {
    server.use(http.get("/api/v1/tasting-readings", () => HttpResponse.error()));
    render(<FlavorPanel flavor={FLAVOR} />);

    expect(await screen.findByText(/tasting readings unavailable/i)).toBeInTheDocument();
    expect(screen.getByText(/balanced for this bean's target/i)).toBeInTheDocument();
  });
});
