import { expect, test } from "@playwright/test";

// Critical workflow 8: comparing a measured shot must make exactly one
// /compare request per click against the real server -- the request-count
// assertion MeasuredShotComparison.test.tsx makes against msw, reproduced
// here against the real network stack.

test("comparing a measured shot issues exactly one compare request", async ({ page }) => {
  await page.goto("/");
  // Audit #06: the "Measured data" tab merged into "Calibration", and the
  // measured-shot picker's own <select> is folded into the shared
  // ground-truth list, so the compare button is what's waited on here.
  await page.getByRole("tab", { name: "Calibration" }).click();
  await expect(page.getByRole("button", { name: /compare with default-v1/i })).toBeVisible();

  let compareRequests = 0;
  page.on("request", (request) => {
    if (/\/api\/v1\/measured-shots\/.+\/compare/.test(request.url())) compareRequests += 1;
  });

  await page.getByRole("button", { name: /compare with default-v1/i }).click();
  await expect(page.getByText(/mass rmse/i)).toBeVisible({ timeout: 15000 });

  expect(compareRequests).toBe(1);
});
