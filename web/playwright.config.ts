import { defineConfig, devices } from "@playwright/test";

// Layer 3-5 of the frontend test plan (docs/testing.md "Frontend runtime
// tests"): real Chromium against a real built native server, not mocks --
// SVG pointer geometry, responsive layout, downloads, and keyboard
// navigation only mean something against actual layout and an actual
// solver round trip. web/e2e/fixtures/native-server.ts documents why the
// server is started here (webServer #1) rather than via scripts/dev.sh.
const NATIVE_SERVER_PORT = Number(process.env.ESPRESSOLAB_E2E_SERVER_PORT ?? 18734);
const VITE_PORT = Number(process.env.ESPRESSOLAB_E2E_VITE_PORT ?? 5183);
const REPO_ROOT = "..";

const isCI = Boolean(process.env.CI);
// The full cross-browser + mobile matrix is nightly-only (section 8's
// "Browser policy"); pull requests get Chromium desktop and mobile.
const fullMatrix = Boolean(process.env.PLAYWRIGHT_FULL_MATRIX);

export default defineConfig({
  testDir: "./e2e",
  fullyParallel: true,
  forbidOnly: isCI,
  // A retry still records the test as flaky rather than silently treating
  // it as healthy -- see the "retries" reporter option below and section 8.
  retries: isCI ? 1 : 0,
  workers: isCI ? 2 : undefined,
  reporter: [
    ["list"],
    ["html", { open: "never" }],
    // Distinguishes a flaky pass-on-retry from a clean first-try pass in the
    // artifact CI uploads, instead of only the terminal summary.
    ["json", { outputFile: "test-results/results.json" }],
  ],

  use: {
    baseURL: `http://127.0.0.1:${VITE_PORT}`,
    trace: "on-first-retry",
    screenshot: "only-on-failure",
    video: "retain-on-failure",
  },

  projects: [
    { name: "chromium", use: { ...devices["Desktop Chrome"] } },
    { name: "chromium-mobile", use: { ...devices["Pixel 7"] } },
    ...(fullMatrix
      ? [
          { name: "firefox", use: { ...devices["Desktop Firefox"] } },
          { name: "webkit", use: { ...devices["Desktop Safari"] } },
          { name: "webkit-mobile", use: { ...devices["iPhone 14"] } },
        ]
      : []),
  ],

  // Two servers, started once for the whole run: the native binary on an
  // isolated port, then Vite with its proxy pointed at that port (see
  // vite.config.ts's ESPRESSOLAB_SERVER_URL). Playwright waits for each
  // `url` to answer before starting the next.
  webServer: [
    {
      command:
        `${REPO_ROOT}/build/apps/espressolab_server/espressolab_server ` +
        `--assets ${REPO_ROOT}/assets --references ${REPO_ROOT}/espresso_real_world_refs ` +
        `--port ${NATIVE_SERVER_PORT}`,
      url: `http://127.0.0.1:${NATIVE_SERVER_PORT}/api/v1/health`,
      reuseExistingServer: !isCI,
      timeout: 30_000,
      stdout: "pipe",
      stderr: "pipe",
    },
    {
      // --host 127.0.0.1 pins the bind address explicitly: Vite's default
      // `localhost` binds to whichever of ::1 / 127.0.0.1 the platform's
      // DNS resolves first, and on this machine that left 127.0.0.1 -- the
      // address Playwright's `url` health check and every test's requests
      // use -- refusing every connection despite Vite reporting "ready".
      command: "npx vite --port " + VITE_PORT + " --host 127.0.0.1 --strictPort",
      url: `http://127.0.0.1:${VITE_PORT}`,
      reuseExistingServer: !isCI,
      timeout: 30_000,
      stdout: "pipe",
      stderr: "pipe",
      // Playwright's webServer `env` REPLACES the child's environment
      // rather than merging with it, so PATH (and everything else `npx`
      // needs to resolve the vite binary) has to be spread in explicitly --
      // omitting this made the command spawn into nothing, silently, with
      // the health check just retrying against a port nothing ever bound.
      env: {
        ...process.env,
        ESPRESSOLAB_SERVER_URL: `http://127.0.0.1:${NATIVE_SERVER_PORT}`,
      },
    },
  ],
});
