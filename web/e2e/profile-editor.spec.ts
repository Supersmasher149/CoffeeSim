import { expect, test } from "@playwright/test";

import { openRecipeEditor } from "./helpers";

// Critical workflow 6: modifying a profile with the keyboard and pointer,
// against real SVG layout -- ProfileCanvas.test.tsx already covers this
// logic in jsdom with an identity coordinate stub; here the actual
// getScreenCTM/createSVGPoint pointer math has to work for real.
//
// The baseline recipe's temperature profile is a single flat point, so its
// "Profile point 1" collides with the pressure profile's -- every locator
// below is scoped to the pressure editor's own group (aria-label "bar
// profile editor") to stay unambiguous.

test("adjusts a pressure-profile point with the keyboard", async ({ page }) => {
  await page.goto("/");
  await openRecipeEditor(page);
  const pressureEditor = page.getByRole("group", { name: "bar profile editor" });
  const firstPoint = pressureEditor.getByRole("slider", { name: "Profile point 1" });
  await expect(firstPoint).toHaveAttribute("aria-valuetext", "0 seconds, 2 bar");

  await firstPoint.focus();
  await page.keyboard.press("ArrowUp");
  await expect(firstPoint).toHaveAttribute("aria-valuetext", "0 seconds, 2.1 bar");

  await page.keyboard.press("ArrowUp");
  await page.keyboard.press("ArrowUp");
  await expect(firstPoint).toHaveAttribute("aria-valuetext", "0 seconds, 2.3 bar");
});

test("drags a pressure-profile point with the pointer", async ({ page }) => {
  await page.goto("/");
  await openRecipeEditor(page);
  const pressureEditor = page.getByRole("group", { name: "bar profile editor" });
  const secondPoint = pressureEditor.getByRole("slider", { name: "Profile point 2" });
  const before = await secondPoint.getAttribute("aria-valuetext");

  await secondPoint.scrollIntoViewIfNeeded();
  const box = await secondPoint.boundingBox();
  if (!box) throw new Error("profile point has no bounding box");
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
  await page.mouse.move(box.x + box.width / 2 + 30, box.y + box.height / 2 - 15, { steps: 5 });
  await page.mouse.up();

  const after = await secondPoint.getAttribute("aria-valuetext");
  expect(after).not.toBe(before);
});

test("edits a profile point through the numeric table and it stays in sync with the graphical control", async ({ page }) => {
  await page.goto("/");
  await openRecipeEditor(page);
  // Audit #08: the numeric points are always visible now, with no disclosure
  // toggle to open first. "Point 1 value in bar" is the pressure profile's
  // field (ControlRail renders the pressure section first).
  const valueInput = page.getByLabel("Point 1 value in bar");
  await valueInput.fill("5");
  await valueInput.blur();

  const pressureEditor = page.getByRole("group", { name: "bar profile editor" });
  await expect(pressureEditor.getByRole("slider", { name: "Profile point 1" })).toHaveAttribute(
    "aria-valuetext",
    "0 seconds, 5 bar",
  );
});
