import type { Page } from "@playwright/test";

export async function openRecipeEditor(page: Page) {
  const toggle = page.getByRole("button", { name: "Edit recipe" });
  if (await toggle.isVisible()) await toggle.click();
}
