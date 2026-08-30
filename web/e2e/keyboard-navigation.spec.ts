import { expect, test } from "@playwright/test";

// Critical workflow 11: the core recipe -> run -> inspect path stays
// reachable by keyboard alone. This does not walk every control (that is
// what the per-component RTL tests are for); it proves the specific chain
// a keyboard-only user needs -- run, then reach the result -- is not
// broken by a positive tabindex, a click-only handler, or a focus trap.

test("runs a simulation and opens diagnostics using only the keyboard", async ({ page }) => {
  await page.goto("/");
  await page.getByRole("button", { name: "Run simulation" }).focus();
  await page.keyboard.press("Enter");

  await expect(page.getByRole("link", { name: "Download CSV" })).toBeVisible({ timeout: 15000 });

  const drawerSummary = page.getByText(/^Diagnostics —/);
  await drawerSummary.focus();
  await page.keyboard.press("Enter");
  await expect(page.getByText(/Mass balance residuals/)).toBeVisible();
});

test("every profile point is reachable by Tab", async ({ page }) => {
  await page.goto("/");
  const pressureEditor = page.getByRole("group", { name: "bar profile editor" });
  const firstPoint = pressureEditor.getByRole("slider", { name: "Profile point 1" });
  await firstPoint.focus();
  await expect(firstPoint).toBeFocused();
  await page.keyboard.press("Tab");
  await expect(pressureEditor.getByRole("slider", { name: "Profile point 2" })).toBeFocused();
});
