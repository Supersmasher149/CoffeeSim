import { defineConfig } from "vitest/config";
import react from "@vitejs/plugin-react";

// Layer 1-2 of the testing pyramid (docs/testing.md "Frontend runtime
// tests"): deterministic unit tests and React Testing Library component
// tests run under jsdom here. SVG pointer behaviour, real layout, downloads
// and native-server workflows are Playwright's job (playwright.config.ts),
// not this file's -- see web/e2e/.
export default defineConfig({
  plugins: [react()],
  test: {
    environment: "jsdom",
    setupFiles: ["./src/test/setup.ts"],
    css: false,
    restoreMocks: true,
    // e2e/ holds Playwright specs, which use @playwright/test's own `test`
    // and must never be collected by Vitest.
    exclude: ["node_modules/**", "e2e/**", "dist/**"],
    coverage: {
      provider: "v8",
      reporter: ["text", "html", "lcov"],
      include: ["src/**/*.{ts,tsx}"],
      exclude: [
        "src/main.tsx",
        "src/**/*.test.{ts,tsx}",
        "src/test/**",
        "src/**/*.d.ts",
      ],
      // Section 7 coverage policy. Recharts-rendered SVG internals and large
      // generated markup are deliberately excluded from what earns coverage
      // credit by not being asserted against in the first place, rather than
      // by carving them out of these numbers.
      thresholds: {
        lines: 75,
        statements: 75,
        functions: 70,
        branches: 65,
        "src/state/workspace.ts": {
          lines: 90,
          statements: 90,
          branches: 85,
        },
        "src/api/client.ts": {
          lines: 90,
          statements: 90,
          branches: 85,
        },
      },
    },
  },
});
