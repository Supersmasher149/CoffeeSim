import { expect, test } from "@playwright/test";

// Critical workflows 9-10: running a small sweep to completion, and
// cancelling a larger one mid-flight to confirm partial results are kept
// and labelled -- against the real background-job/poll lifecycle the
// native server implements, not SweepPanel.test.tsx's mocked timings.

test("runs a small sweep to completion and shows its results table", async ({ page }) => {
  await page.goto("/");
  const stepsInputs = page.getByLabel("steps");
  await stepsInputs.first().fill("3");

  await page.getByRole("button", { name: /run sweep \(3\)/i }).click();
  await expect(page.getByRole("link", { name: /download aggregate csv/i })).toBeVisible({ timeout: 30000 });

  const table = page.getByRole("table").last();
  await expect(table.getByRole("row")).toHaveCount(1 + 3); // header + 3 runs
});

test("cancelling a running sweep keeps partial results, labelled as partial", async ({ page }) => {
  await page.goto("/");
  const stepsInputs = page.getByLabel("steps");
  await stepsInputs.first().fill("40");

  await page.getByRole("button", { name: /run sweep \(40\)/i }).click();
  await expect(page.getByRole("button", { name: /^cancel$/i })).toBeVisible({ timeout: 10000 });
  await page.getByRole("button", { name: /^cancel$/i }).click();

  await expect(page.getByText(/partial results kept/i)).toBeVisible({ timeout: 30000 });
  const table = page.getByRole("table").last();
  const rowCount = await table.getByRole("row").count();
  // Header row plus at least one, but fewer than the full 40 requested runs.
  expect(rowCount).toBeGreaterThan(1);
  expect(rowCount).toBeLessThan(1 + 40);
});
