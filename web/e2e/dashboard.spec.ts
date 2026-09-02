import { expect, test } from "@playwright/test";
import * as fs from "node:fs/promises";

import { openRecipeEditor } from "./helpers";

// Critical workflows 1-5 of the frontend test plan, against the real
// built native server (playwright.config.ts's webServer array) -- not
// mocks. Component tests already cover every branch of this logic in
// jsdom; this file only has to prove the real DOM/network/download path
// actually works end to end.

test("loads the dashboard and the baseline catalogue", async ({ page }) => {
  await page.goto("/");
  await openRecipeEditor(page);
  await expect(page.getByRole("heading", { name: "EspressoLab" })).toBeVisible();
  const recipeSelect = page.getByRole("combobox", { name: "Recipe" });
  // Several catalogue entries have "baseline" in their id (per-machine
  // variants); the dashboard's default selection is the exact-named entry.
  await expect(recipeSelect.getByRole("option", { name: "Baseline 18 g espresso" })).toHaveCount(1);
  await expect(recipeSelect).toHaveValue("baseline");
  await expect(page.getByText(/solver-/)).toBeVisible();
});

test("edits a recipe and runs a simulation, showing metrics and diagnostics", async ({ page }) => {
  await page.goto("/");
  await openRecipeEditor(page);
  const doseField = page.getByLabel("Dose (g)");
  await expect(doseField).toHaveValue("18");
  await doseField.fill("20");

  await page.getByRole("button", { name: "Run simulation" }).click();
  const csvLink = page.getByRole("link", { name: "Download CSV" });
  await expect(csvLink).toBeVisible({ timeout: 15000 });

  await expect(page.locator(".metric-strip").getByText("Shot time", { exact: true })).toBeVisible();
  await expect(page.locator(".metric-strip")).toContainText("g"); // beverage mass metric

  const drawer = page.getByText(/^Diagnostics —/);
  await drawer.click();
  await expect(page.getByText(/Mass balance residuals/)).toBeVisible();
  await expect(page.getByText(/run id/)).toBeVisible();
});

test("the shot analysis chart appears while the puck transport is still playing", async ({ page }) => {
  // Regression: ChartStack used to be a lazy import behind a Suspense
  // boundary that sat beside PuckView. A completed run auto-plays PuckView's
  // transport, which sets state from requestAnimationFrame on every frame for
  // the length of the shot; those default-priority updates starved React's
  // retry of the suspended boundary, so the charts stayed behind
  // "Loading charts..." until playback ended (29 s on the baseline recipe) or
  // the reader hit Pause. The assertions below all run while the transport is
  // still playing, which is what made the old code fail.
  await page.goto("/");
  await openRecipeEditor(page);
  await page.getByRole("button", { name: "Run simulation" }).click();
  await expect(page.locator(".metric-strip")).toBeVisible({ timeout: 15000 });

  // "Pause" is the transport's label only while it is playing.
  await expect(page.getByRole("button", { name: "Pause" })).toBeVisible();
  await expect(page.locator(".analysis-canvas-well")).toBeVisible({ timeout: 5000 });
  await expect(page.getByText("Loading charts...")).toHaveCount(0);
  await expect(page.locator(".analysis-canvas-svg path[data-series='pressure']")).toHaveCount(1);
  // Still playing: the chart arrived without the animation having to finish.
  await expect(page.getByRole("button", { name: "Pause" })).toBeVisible();
});

test("two runs of the identical recipe produce the identical result hash (determinism)", async ({ page }) => {
  await page.goto("/");
  await openRecipeEditor(page);
  await page.getByRole("button", { name: "Run simulation" }).click();
  await expect(page.getByRole("link", { name: "Download CSV" })).toBeVisible({ timeout: 15000 });
  await page.getByText(/^Diagnostics —/).click();
  const firstHash = await page.locator(".kv", { hasText: "result hash" }).locator("span").last().textContent();

  await page.reload();
  await openRecipeEditor(page);
  await page.getByRole("button", { name: "Run simulation" }).click();
  await expect(page.getByRole("link", { name: "Download CSV" })).toBeVisible({ timeout: 15000 });
  await page.getByText(/^Diagnostics —/).click();
  const secondHash = await page.locator(".kv", { hasText: "result hash" }).locator("span").last().textContent();

  expect(firstHash).toBeTruthy();
  expect(firstHash).toBe(secondHash);
});

test("downloads JSON and CSV artifacts for a completed run", async ({ page }) => {
  await page.goto("/");
  await openRecipeEditor(page);
  await page.getByRole("button", { name: "Run simulation" }).click();
  await expect(page.getByRole("link", { name: "Download CSV" })).toBeVisible({ timeout: 15000 });

  const [csvDownload] = await Promise.all([
    page.waitForEvent("download"),
    page.getByRole("link", { name: "Download CSV" }).click(),
  ]);
  expect(csvDownload.suggestedFilename()).toMatch(/^shot-.*\.csv$/);
  const csvPath = await csvDownload.path();
  const csvContent = await fs.readFile(csvPath!, "utf8");
  expect(csvContent.split("\n")[0]).toContain("time_s"); // header row

  const [jsonDownload] = await Promise.all([
    page.waitForEvent("download"),
    page.getByRole("button", { name: "Download JSON" }).click(),
  ]);
  expect(jsonDownload.suggestedFilename()).toMatch(/^shot-.*\.json$/);
  const jsonPath = await jsonDownload.path();
  const parsed = JSON.parse(await fs.readFile(jsonPath!, "utf8"));
  expect(parsed.manifest.run_id).toMatch(/^shot-/);
  expect(Array.isArray(parsed.samples)).toBe(true);
});

test("pinning a run keeps its label correct after the draft is edited (Audit P7 issue #22)", async ({ page }) => {
  await page.goto("/");
  await openRecipeEditor(page);
  await page.getByRole("button", { name: "Run simulation" }).click();
  await expect(page.getByRole("link", { name: "Download CSV" })).toBeVisible({ timeout: 15000 });

  await page.getByRole("button", { name: "Pin current run" }).click();
  await expect(page.getByText(/current run is already pinned/i)).toBeVisible();

  // Edit the draft after pinning; the reference panel's "current model"
  // column must still show the submitted dose (18g), not the new draft.
  await page.getByLabel("Dose (g)").fill("14");

  // Audit #06: References merged into the Calibration tab's shared
  // ground-truth list; select the first published reference row to see its
  // metadata table (the real catalogue's reference ids vary by asset file,
  // unlike the vitest fixture's fixed "real_gagne_shot_01").
  await page.getByRole("tab", { name: "Calibration" }).click();
  await page.getByRole("button", { name: /published/i }).first().click();
  const referenceTable = page.getByRole("table");
  const doseRow = referenceTable.getByRole("row").filter({ hasText: "Dose" });
  await expect(doseRow.locator(".current-model")).toHaveText("18.0 g");
});
