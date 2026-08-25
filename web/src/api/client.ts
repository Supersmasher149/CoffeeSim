import type { ApiError, Recipe, ShotResult, SweepResult, ValidationIssue } from "./types";

const BASE = "/api/v1";

export class ApiFailure extends Error {
  readonly code: string;
  readonly path: string;
  readonly issues: ValidationIssue[];

  constructor(body: ApiError) {
    super(body.error.message);
    this.code = body.error.code;
    this.path = body.error.path;
    const details = body.error.details as { issues?: ValidationIssue[] } | undefined;
    this.issues = details?.issues ?? [
      { code: body.error.code, message: body.error.message, path: body.error.path },
    ];
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(`${BASE}${path}`, {
    headers: { "Content-Type": "application/json" },
    ...init,
  });
  if (!response.ok) {
    // The server always answers with the section 12.2 error contract; anything
    // else means the tool server is not the thing on the other end.
    const body = (await response.json().catch(() => null)) as ApiError | null;
    if (body?.error) throw new ApiFailure(body);
    throw new Error(`${response.status} ${response.statusText}`);
  }
  return (await response.json()) as T;
}

export interface HealthResponse {
  status: string;
  solver_version: string;
  recipe_schema_version: string;
  result_schema_version: string;
  asset_root: string;
  sweepable_parameters: string[];
}

export const api = {
  health: () => request<HealthResponse>("/health"),

  recipes: () => request<{ recipes: { id: string; name: string; recipe: Recipe }[] }>("/recipes"),

  simulate: (recipe: Recipe) =>
    request<ShotResult>("/shots", { method: "POST", body: JSON.stringify({ recipe }) }),

  sweep: (name: string, baseline: Recipe, axes: { parameter_path: string; values: number[] }[]) =>
    request<SweepResult>("/sweeps", {
      method: "POST",
      body: JSON.stringify({ name, baseline, axes }),
    }),

  csvUrl: (id: string) => `${BASE}/artifacts/${id}.csv`,
};
