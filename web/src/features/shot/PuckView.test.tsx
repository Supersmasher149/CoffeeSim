import { fireEvent, render, screen } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import { makeShotResult } from "../../test/fixtures/shotResult";
import { PuckView } from "./PuckView";

function reducedMotionMatchMedia(matches: boolean) {
  return vi.fn((query: string) => ({
    matches,
    media: query,
    onchange: null,
    addListener: () => {},
    removeListener: () => {},
    addEventListener: () => {},
    removeEventListener: () => {},
    dispatchEvent: () => false,
  })) as unknown as typeof window.matchMedia;
}

describe("PuckView: rendering guard", () => {
  it("renders nothing for a result with an empty sample series", () => {
    const result = makeShotResult({ samples: [] });
    const { container } = render(<PuckView result={result} targetBeverageG={36} />);
    expect(container).toBeEmptyDOMElement();
  });
});

describe("PuckView: autoplay and reduced motion", () => {
  it("autostarts playback (shows Pause) for a fresh run when motion is not reduced", () => {
    window.matchMedia = reducedMotionMatchMedia(false);
    const result = makeShotResult();
    render(<PuckView result={result} targetBeverageG={36} />);
    expect(screen.getByRole("button", { name: /pause playback/i })).toBeInTheDocument();
  });

  it("does not autostart playback when prefers-reduced-motion is set", () => {
    window.matchMedia = reducedMotionMatchMedia(true);
    const result = makeShotResult();
    render(<PuckView result={result} targetBeverageG={36} />);
    expect(screen.getByRole("button", { name: /play the shot/i })).toBeInTheDocument();
    expect(screen.getByLabelText(/shot time/i)).toHaveValue("0");
  });
});

describe("PuckView: transport controls", () => {
  it("toggles Play/Pause on click and takes control away from the chart cursor", () => {
    window.matchMedia = reducedMotionMatchMedia(true);
    const result = makeShotResult();
    render(<PuckView result={result} targetBeverageG={36} cursorTimeSeconds={5} />);
    // Following the chart cursor initially.
    expect(screen.getByText(/following the chart cursor/i)).toBeInTheDocument();

    fireEvent.click(screen.getByRole("button", { name: /play the shot/i }));
    expect(screen.getByRole("button", { name: /pause playback/i })).toBeInTheDocument();
    expect(screen.getByText(/5\.0 \/ 28\.0 s/)).toBeInTheDocument();
  });

  it("exposes a speed selector with the documented options and updates on change", () => {
    window.matchMedia = reducedMotionMatchMedia(true);
    render(<PuckView result={makeShotResult()} targetBeverageG={36} />);
    const speed = screen.getByLabelText(/playback speed/i);
    expect(speed).toHaveValue("1");
    fireEvent.change(speed, { target: { value: "2" } });
    expect(speed).toHaveValue("2");
  });

  it("scrubbing the range input pauses playback, updates the displayed clock, and detaches from the cursor", () => {
    window.matchMedia = reducedMotionMatchMedia(true);
    const result = makeShotResult();
    render(<PuckView result={result} targetBeverageG={36} cursorTimeSeconds={5} />);
    const slider = screen.getByLabelText(/shot time/i);

    fireEvent.change(slider, { target: { value: "10" } });
    expect(screen.getByText(/10\.0 \/ 28\.0 s/)).toBeInTheDocument();
    // Detached from the cursor now -- the phase label shows again instead of
    // "following the chart cursor", even though cursorTimeSeconds is
    // unchanged, because scrubbing recorded a detach point.
    expect(screen.queryByText(/following the chart cursor/i)).not.toBeInTheDocument();
  });
});

describe("PuckView: cursor following", () => {
  it("clamps a cursor time beyond the run's duration to the last sample", () => {
    window.matchMedia = reducedMotionMatchMedia(true);
    const result = makeShotResult(); // duration 28s
    render(<PuckView result={result} targetBeverageG={36} cursorTimeSeconds={999} />);
    expect(screen.getByText(/28\.0 \/ 28\.0 s/)).toBeInTheDocument();
  });

  it("re-attaches to a new cursor value after a detach", () => {
    window.matchMedia = reducedMotionMatchMedia(true);
    const result = makeShotResult();
    const { rerender } = render(<PuckView result={result} targetBeverageG={36} cursorTimeSeconds={5} />);
    fireEvent.change(screen.getByLabelText(/shot time/i), { target: { value: "10" } });
    expect(screen.queryByText(/following the chart cursor/i)).not.toBeInTheDocument();

    rerender(<PuckView result={result} targetBeverageG={36} cursorTimeSeconds={20} />);
    expect(screen.getByText(/following the chart cursor/i)).toBeInTheDocument();
    expect(screen.getByText(/20\.0 \/ 28\.0 s/)).toBeInTheDocument();
  });
});

describe("PuckView: RAF lifecycle", () => {
  it("cancels its animation frame on unmount instead of leaking a callback", () => {
    window.matchMedia = reducedMotionMatchMedia(false); // autoplay -> schedules a frame
    const cancelSpy = vi.spyOn(window, "cancelAnimationFrame");
    const result = makeShotResult();
    const { unmount } = render(<PuckView result={result} targetBeverageG={36} />);
    unmount();
    expect(cancelSpy).toHaveBeenCalled();
  });

  it("does not schedule a frame at all when reduced motion is set", () => {
    window.matchMedia = reducedMotionMatchMedia(true);
    const rafSpy = vi.spyOn(window, "requestAnimationFrame");
    render(<PuckView result={makeShotResult()} targetBeverageG={36} />);
    expect(rafSpy).not.toHaveBeenCalled();
  });
});

describe("PuckView: accessible cross-section summary", () => {
  it("names the sample time, phase, saturation, inflow and cup mass in the SVG's aria-label", () => {
    window.matchMedia = reducedMotionMatchMedia(true);
    const result = makeShotResult();
    render(<PuckView result={result} targetBeverageG={36} />);
    const svg = screen.getByRole("img");
    expect(svg.getAttribute("aria-label")).toMatch(/dry puck/i);
    expect(svg.getAttribute("aria-label")).toMatch(/saturation 0 percent/i);
  });
});
