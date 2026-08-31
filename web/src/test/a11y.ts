import { axe } from "vitest-axe";
import type { AxeMatchers } from "vitest-axe/dist/matchers.js";
import { expect } from "vitest";

// The root "vitest-axe/matchers" shim's own types are type-only re-exports
// (see setup.ts), so the Assertion interface augmentation lives here instead
// of coming from the package's own (broken) extend-expect entry point.
declare module "vitest" {
  interface Assertion<T = any> extends AxeMatchers {}
}

// Section 4 of the frontend test plan: fail CI on serious/critical
// accessibility violations only. Axe's own "moderate"/"minor" findings are
// real but noisier and more debatable; gating on those would make this
// check flaky for reasons unrelated to a genuine regression, so this filters
// down to the two impact levels worth a hard failure before ever calling
// the matcher.
export async function expectNoSeriousViolations(container: Element) {
  const results = await axe(container);
  const serious = results.violations.filter(
    (violation) => violation.impact === "serious" || violation.impact === "critical",
  );
  expect({ ...results, violations: serious }).toHaveNoViolations();
}
