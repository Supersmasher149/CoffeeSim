import { http, HttpResponse } from "msw";
import { describe, expect, it } from "vitest";

import { server } from "../test/fixtures/server";
import { makeRecipe } from "../test/fixtures/recipe";
import { api, ApiFailure } from "./client";
import type { ApiError } from "./types";

function errorBody(overrides: Partial<ApiError["error"]> = {}): ApiError {
  return {
    error: { code: "OUT_OF_RANGE", message: "recipe.puck.dose_g is out of range", path: "recipe.puck.dose_g", ...overrides },
  };
}

describe("api client: HTTP methods, paths and bodies", () => {
  it("health() issues a GET against /api/v1/health", async () => {
    let seenMethod: string | undefined;
    server.use(
      http.get("/api/v1/health", ({ request }) => {
        seenMethod = request.method;
        return HttpResponse.json({ status: "ok" });
      }),
    );
    await api.health();
    expect(seenMethod).toBe("GET");
  });

  it("recipes() GETs /api/v1/recipes and returns the parsed body", async () => {
    server.use(
      http.get("/api/v1/recipes", () => HttpResponse.json({ recipes: [{ id: "baseline", name: "x", recipe: makeRecipe() }] })),
    );
    const body = await api.recipes();
    expect(body.recipes).toHaveLength(1);
    expect(body.recipes[0].id).toBe("baseline");
  });

  it("simulate() POSTs the recipe wrapped in {recipe} with a JSON body", async () => {
    let seenMethod: string | undefined;
    let seenBody: unknown;
    let seenContentType: string | null = null;
    const recipe = makeRecipe();
    server.use(
      http.post("/api/v1/shots", async ({ request }) => {
        seenMethod = request.method;
        seenContentType = request.headers.get("content-type");
        seenBody = await request.json();
        return HttpResponse.json({ manifest: { run_id: "run-0001" } });
      }),
    );
    await api.simulate(recipe);
    expect(seenMethod).toBe("POST");
    expect(seenContentType).toContain("application/json");
    expect(seenBody).toEqual({ recipe });
  });

  it("startSweep() POSTs name, baseline and axes", async () => {
    let seenBody: unknown;
    server.use(
      http.post("/api/v1/sweeps", async ({ request }) => {
        seenBody = await request.json();
        return HttpResponse.json({ sweep_id: "sweep-1", status: "queued", completed: 0, total: 9, poll: "" });
      }),
    );
    const baseline = makeRecipe();
    await api.startSweep("dashboard", baseline, [{ parameter_path: "puck.dose_g", values: [16, 18, 20] }]);
    expect(seenBody).toEqual({
      name: "dashboard",
      baseline,
      axes: [{ parameter_path: "puck.dose_g", values: [16, 18, 20] }],
    });
  });

  it("cancelSweep() POSTs to /sweeps/:id/cancel", async () => {
    let seenMethod: string | undefined;
    let seenPath: string | undefined;
    server.use(
      http.post("/api/v1/sweeps/:id/cancel", ({ request, params }) => {
        seenMethod = request.method;
        seenPath = params.id as string;
        return HttpResponse.json({ sweep_id: "sweep-7", cancel_requested: true });
      }),
    );
    const result = await api.cancelSweep("sweep-7");
    expect(seenMethod).toBe("POST");
    expect(seenPath).toBe("sweep-7");
    expect(result.cancel_requested).toBe(true);
  });

  it("sweepStatus() GETs /sweeps/:id", async () => {
    server.use(
      http.get("/api/v1/sweeps/:id", ({ params }) =>
        HttpResponse.json({ sweep_id: params.id, status: "running", completed: 1, total: 4, elapsed_s: 0.5 }),
      ),
    );
    const result = await api.sweepStatus("sweep-9");
    expect(result.sweep_id).toBe("sweep-9");
  });

  it("cfd3dRun() POSTs the run request as-is", async () => {
    let seenBody: unknown;
    server.use(
      http.post("/api/v1/cfd3d/runs", async ({ request }) => {
        seenBody = await request.json();
        return HttpResponse.json({ run_id: "cfd3d-1", status: "queued", poll: "" });
      }),
    );
    const run = { recipe: makeRecipe(), mesh: { nx: 4, ny: 4, nz: 4 } };
    await api.cfd3dRun(run);
    expect(seenBody).toEqual(run);
  });

  it("cfd3dStatus() GETs /cfd3d/runs/:id", async () => {
    server.use(
      http.get("/api/v1/cfd3d/runs/:id", ({ params }) =>
        HttpResponse.json({ run_id: params.id, status: "complete", snapshot_count: 2, elapsed_s: 1 }),
      ),
    );
    const result = await api.cfd3dStatus("cfd3d-2");
    expect(result.run_id).toBe("cfd3d-2");
  });

  it("cfd3dSnapshot() encodes the field query parameter and defaults to saturation", async () => {
    let seenUrl = "";
    server.use(
      http.get("/api/v1/cfd3d/runs/:id/snapshots/:index", ({ request }) => {
        seenUrl = request.url;
        return HttpResponse.json({
          run_id: "cfd3d-2", snapshot_index: 0, time_s: 0, field: "saturation",
          mesh: { nx: 1, ny: 1, nz: 1 }, ordering: "x-fastest, then y, then z", values: [0],
        });
      }),
    );
    await api.cfd3dSnapshot("cfd3d-2", 3);
    expect(seenUrl).toContain("/snapshots/3?field=saturation");

    await api.cfd3dSnapshot("cfd3d-2", 3, "pressure_pa");
    expect(seenUrl).toContain("field=pressure_pa");
  });

  it("csvUrl() builds an artifact path without a network request", () => {
    expect(api.csvUrl("run-0001")).toBe("/api/v1/artifacts/run-0001.csv");
    expect(api.csvUrl("sweep-0001")).toBe("/api/v1/artifacts/sweep-0001.csv");
  });
});

describe("api client: URL encoding of identifiers and query values", () => {
  it("encodes a measured-shot identifier with reserved characters in the path", async () => {
    let seenId = "";
    let seenQuery = "";
    server.use(
      http.get("/api/v1/measured-shots/:id/compare", ({ params, request }) => {
        seenId = params.id as string;
        seenQuery = new URL(request.url).searchParams.get("coefficients") ?? "";
        return HttpResponse.json({});
      }),
    );
    await api.compareMeasuredShot("shot/weird id?#01", "coeff set v1&2", new AbortController().signal);
    // msw decodes :id path params, so the round trip through encodeURIComponent
    // must land back on the exact identifier that was passed in.
    expect(seenId).toBe("shot/weird id?#01");
    expect(seenQuery).toBe("coeff set v1&2");
  });

  it("encodes a coefficient selector containing '+' and '/' ", async () => {
    let seenQuery = "";
    server.use(
      http.get("/api/v1/measured-shots/:id/compare", ({ request }) => {
        seenQuery = new URL(request.url).searchParams.get("coefficients") ?? "";
        return HttpResponse.json({});
      }),
    );
    await api.compareMeasuredShot("shot-1", "a+b/c", new AbortController().signal);
    expect(seenQuery).toBe("a+b/c");
  });
});

describe("api client: AbortSignal forwarding", () => {
  it("measuredShots() rejects immediately when the signal is already aborted", async () => {
    const controller = new AbortController();
    controller.abort();
    await expect(api.measuredShots(controller.signal)).rejects.toMatchObject({ name: "AbortError" });
  });

  it("compareMeasuredShot() rejects when its signal is aborted", async () => {
    server.use(
      http.get("/api/v1/measured-shots/:id/compare", () => HttpResponse.json({})),
    );
    const controller = new AbortController();
    controller.abort();
    await expect(
      api.compareMeasuredShot("shot-1", "default-v1", controller.signal),
    ).rejects.toMatchObject({ name: "AbortError" });
  });

  it("an unaborted signal does not prevent a normal response", async () => {
    server.use(
      http.get("/api/v1/measured-shots", () => HttpResponse.json({ schema_version: "1.0", measured_shots: [], count: 0 })),
    );
    const controller = new AbortController();
    await expect(api.measuredShots(controller.signal)).resolves.toMatchObject({ count: 0 });
  });
});

describe("api client: structured error parsing", () => {
  it("throws ApiFailure with code/path/issues for a structured error response", async () => {
    server.use(
      http.post(
        "/api/v1/shots",
        () =>
          HttpResponse.json(
            errorBody({
              details: { issues: [{ code: "OUT_OF_RANGE", path: "recipe.puck.dose_g", message: "too low" }] },
            }),
            { status: 422 },
          ),
      ),
    );
    const failure = await api.simulate(makeRecipe()).catch((e) => e);
    expect(failure).toBeInstanceOf(ApiFailure);
    expect(failure.code).toBe("OUT_OF_RANGE");
    expect(failure.path).toBe("recipe.puck.dose_g");
    expect(failure.issues).toEqual([{ code: "OUT_OF_RANGE", path: "recipe.puck.dose_g", message: "too low" }]);
    expect(failure.message).toBe("recipe.puck.dose_g is out of range");
  });

  it("synthesizes a single-issue array when the error has no details.issues", async () => {
    server.use(
      http.post("/api/v1/shots", () => HttpResponse.json(errorBody(), { status: 400 })),
    );
    const failure = await api.simulate(makeRecipe()).catch((e) => e);
    expect(failure).toBeInstanceOf(ApiFailure);
    expect(failure.issues).toEqual([
      { code: "OUT_OF_RANGE", message: "recipe.puck.dose_g is out of range", path: "recipe.puck.dose_g" },
    ]);
  });

  it("synthesizes a single-issue array when details is present but has no issues field", async () => {
    server.use(
      http.post("/api/v1/shots", () =>
        HttpResponse.json(errorBody({ details: { hint: "check the dose" } }), { status: 400 }),
      ),
    );
    const failure = await api.simulate(makeRecipe()).catch((e) => e);
    expect(failure.issues).toHaveLength(1);
    expect(failure.issues[0].code).toBe("OUT_OF_RANGE");
  });

  it("falls back to a plain Error with the HTTP status when the error body is not JSON", async () => {
    server.use(
      http.post("/api/v1/shots", () => new HttpResponse("<html>not json</html>", { status: 500, statusText: "Internal Server Error" })),
    );
    const failure = await api.simulate(makeRecipe()).catch((e) => e);
    expect(failure).not.toBeInstanceOf(ApiFailure);
    expect(failure).toBeInstanceOf(Error);
    expect(failure.message).toBe("500 Internal Server Error");
  });

  it("falls back to a plain Error when the failure body is JSON but has no `error` field", async () => {
    server.use(
      http.post("/api/v1/shots", () =>
        HttpResponse.json({ unexpected: true }, { status: 400, statusText: "Bad Request" }),
      ),
    );
    const failure = await api.simulate(makeRecipe()).catch((e) => e);
    expect(failure).not.toBeInstanceOf(ApiFailure);
    expect(failure.message).toBe("400 Bad Request");
  });

  it("rejects when the network request itself fails", async () => {
    server.use(http.get("/api/v1/health", () => HttpResponse.error()));
    await expect(api.health()).rejects.toBeTruthy();
  });
});
