import { http, HttpResponse } from "msw";
import { setupServer } from "msw/node";

import { makeCatalogue } from "./recipe";
import { makeReferenceCatalogue, makeHealth } from "./reference";
import { makeMeasuredShotCatalogue } from "./measuredShot";

// Default handlers cover the requests App.tsx fires on mount so most
// component tests only need to override the one endpoint their scenario
// cares about (server.use(...)), rather than restate the whole surface.
export const defaultHandlers = [
  http.get("/api/v1/health", () => HttpResponse.json(makeHealth())),
  http.get("/api/v1/recipes", () => HttpResponse.json({ recipes: makeCatalogue() })),
  http.get("/api/v1/reference-shots", () => HttpResponse.json(makeReferenceCatalogue())),
  http.get("/api/v1/measured-shots", () => HttpResponse.json(makeMeasuredShotCatalogue())),
];

export const server = setupServer(...defaultHandlers);
