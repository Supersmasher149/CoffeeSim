import "@testing-library/jest-dom/vitest";
import { afterAll, afterEach, beforeAll, beforeEach, expect, vi } from "vitest";
import { cleanup } from "@testing-library/react";
// vitest-axe's root-level "vitest-axe/matchers" shim re-exports this as a
// type-only `export type *`, so the value has to come from the dist path
// directly -- see the type augmentation in src/test/a11y.ts.
import { toHaveNoViolations } from "vitest-axe/dist/matchers.js";

import { server } from "./fixtures/server";

// vitest-axe 0.1.0 ships a broken (empty) extend-expect.js, so the matcher
// is registered by hand here instead of via its documented import path.
expect.extend({ toHaveNoViolations });

// Component tests hit the real fetch() call path through src/api/client.ts
// against this mock server rather than mocking the client module itself, so
// they exercise the same request/response parsing App.tsx depends on.
// `onUnhandledRequest: "error"` means a component reaching an endpoint no
// test wired up fails loudly instead of hanging on a real network call.
beforeAll(() => server.listen({ onUnhandledRequest: "error" }));
afterEach(() => server.resetHandlers());
afterAll(() => server.close());

// This file is Vitest's setupFiles entry (vitest.config.ts). It exists so
// every unit/component test gets a jsdom environment that looks enough like
// a browser for our own components' logic to run -- not to make Recharts'
// internals pixel-accurate. Chart geometry, real layout and SVG pointer math
// stay Playwright's job (web/e2e/).

afterEach(() => {
  cleanup();
});

// jsdom has no layout engine: every element reports 0x0. Recharts'
// <ResponsiveContainer> measures its wrapper div and renders nothing at
// 0x0, which would make every chart test see an empty tree regardless of
// what data it was given. A fixed, generous size lets charts mount their
// series/markers so component tests can assert on those without depending
// on real pixel geometry (still Playwright's job).
const CHART_WIDTH = 800;
const CHART_HEIGHT = 400;

function stubDimensions(target: typeof HTMLElement.prototype) {
  Object.defineProperty(target, "offsetWidth", { configurable: true, get: () => CHART_WIDTH });
  Object.defineProperty(target, "offsetHeight", { configurable: true, get: () => CHART_HEIGHT });
  Object.defineProperty(target, "clientWidth", { configurable: true, get: () => CHART_WIDTH });
  Object.defineProperty(target, "clientHeight", { configurable: true, get: () => CHART_HEIGHT });
}
stubDimensions(HTMLElement.prototype);

Element.prototype.getBoundingClientRect = function getBoundingClientRect() {
  return {
    width: CHART_WIDTH,
    height: CHART_HEIGHT,
    top: 0,
    left: 0,
    right: CHART_WIDTH,
    bottom: CHART_HEIGHT,
    x: 0,
    y: 0,
    toJSON() {
      return this;
    },
  } as DOMRect;
};

// jsdom implements neither ResizeObserver nor the SVG measurement APIs
// Recharts and d3 reach for. Firing the callback once, synchronously, is
// enough for ResponsiveContainer to settle on the stubbed size above without
// pretending to track real resizes. This is a plain class (not vi.fn()), so
// `restoreMocks` in vitest.config.ts leaves it alone between tests.
class ResizeObserverStub {
  private readonly callback: ResizeObserverCallback;
  constructor(callback: ResizeObserverCallback) {
    this.callback = callback;
  }
  observe(target: Element) {
    this.callback(
      [{ target, contentRect: target.getBoundingClientRect() } as ResizeObserverEntry],
      this as unknown as ResizeObserver,
    );
  }
  unobserve() {}
  disconnect() {}
}
vi.stubGlobal("ResizeObserver", ResizeObserverStub);

type SvgMeasurement = { getBBox?: () => DOMRect; getComputedTextLength?: () => number };
const svgProto = SVGElement.prototype as unknown as SvgMeasurement;
svgProto.getBBox ??= () => ({ x: 0, y: 0, width: 0, height: 0 }) as DOMRect;
svgProto.getComputedTextLength ??= () => 0;

// ProfileCanvas maps a pointer event to SVG user space via createSVGPoint +
// getScreenCTM().inverse(), neither of which jsdom implements. The identity
// mapping below means a synthetic pointer event's clientX/clientY *is* the
// SVG user-space coordinate a test asserts against -- real CTM/viewport
// scaling from an actual layout stays Playwright's job (web/e2e/); this only
// has to exercise ProfileCanvas's own snapping/ordering/rejection logic.
type SvgPoint = { x: number; y: number; matrixTransform: (m: unknown) => { x: number; y: number } };
type SvgCoordinates = {
  createSVGPoint?: () => SvgPoint;
  getScreenCTM?: () => { inverse: () => unknown } | null;
};
const svgSvgProto = SVGSVGElement.prototype as unknown as SvgCoordinates;
svgSvgProto.createSVGPoint ??= () => ({
  x: 0,
  y: 0,
  matrixTransform(this: SvgPoint) {
    return { x: this.x, y: this.y };
  },
});
svgSvgProto.getScreenCTM ??= () => ({ inverse: () => ({}) });

// jsdom has no requestAnimationFrame loop; drive it off a macrotask so
// playback/scrub code still advances when a test flushes timers (real or
// fake). Plain functions, not vi.fn(), for the same restoreMocks reason.
vi.stubGlobal("requestAnimationFrame", (cb: FrameRequestCallback) =>
  setTimeout(() => cb(performance.now()), 16),
);
vi.stubGlobal("cancelAnimationFrame", (id: number) => clearTimeout(id));

// vitest.config.ts sets `restoreMocks: true` so every test starts from a
// clean slate -- but that also unwinds a vi.fn()'s *implementation* back to
// a no-op, not just its call history. matchMedia and the Blob URL functions
// below are wired up per-test, in beforeEach (which vitest runs after its
// own restore hook), rather than once at module load, so they still work on
// every test rather than only the first.
let objectUrlSequence = 0;

beforeEach(() => {
  objectUrlSequence = 0;
  URL.createObjectURL = vi.fn(() => `blob:mock-${++objectUrlSequence}`);
  URL.revokeObjectURL = vi.fn();

  // PuckView reads prefers-reduced-motion; jsdom has no matchMedia at all.
  window.matchMedia = vi.fn(
    (query: string) =>
      ({
        matches: false,
        media: query,
        onchange: null,
        addListener: () => {},
        removeListener: () => {},
        addEventListener: () => {},
        removeEventListener: () => {},
        dispatchEvent: () => false,
      }) as MediaQueryList,
  );
});
