import { useId, useRef, useState } from "react";

import type { ProfilePoint } from "../../api/types";

interface Props {
  points: ProfilePoint[];
  range: readonly [number, number];
  maxTimeSeconds: number;
  unit: string;
  color: string;
  onChange: (points: ProfilePoint[]) => void;
}

const VIEW = { width: 320, height: 132 };
const PAD = { left: 30, right: 8, top: 10, bottom: 20 };

const PLOT_WIDTH = VIEW.width - PAD.left - PAD.right;
const PLOT_HEIGHT = VIEW.height - PAD.top - PAD.bottom;

// Dragging edits the same ordered time/value points the numeric list edits, so
// the graphical control is a second view of one model rather than a second
// model (15.2 keeps the numeric points when this control is cut).
const TIME_SNAP = 0.5;
const MIN_GAP_S = 0.5;

export function ProfileCanvas({ points, range, maxTimeSeconds, unit, color, onChange }: Props) {
  const svgRef = useRef<SVGSVGElement>(null);
  const instructionsId = useId();
  const [dragIndex, setDragIndex] = useState<number>();
  const [hoverIndex, setHoverIndex] = useState<number>();

  const span = maxTimeSeconds || 1;
  const [low, high] = range;

  const toX = (time: number) => PAD.left + (Math.min(time, span) / span) * PLOT_WIDTH;
  const toY = (value: number) =>
    PAD.top + PLOT_HEIGHT - ((value - low) / (high - low || 1)) * PLOT_HEIGHT;

  const fromEvent = (event: React.PointerEvent | React.MouseEvent) => {
    const svg = svgRef.current;
    if (!svg) return undefined;
    const point = svg.createSVGPoint();
    point.x = event.clientX;
    point.y = event.clientY;
    const screen = svg.getScreenCTM();
    if (!screen) return undefined;
    const local = point.matrixTransform(screen.inverse());
    const time = ((local.x - PAD.left) / PLOT_WIDTH) * span;
    const value = low + ((PAD.top + PLOT_HEIGHT - local.y) / PLOT_HEIGHT) * (high - low);
    return { time, value };
  };

  const snapTime = (time: number) =>
    Math.max(0, Math.round(time / TIME_SNAP) * TIME_SNAP);
  const snapValue = (value: number) =>
    Math.min(high, Math.max(low, Math.round(value * 10) / 10));

  const handleMove = (event: React.PointerEvent) => {
    if (dragIndex === undefined) return;
    const position = fromEvent(event);
    if (!position) return;

    // Times must stay strictly increasing, so a dragged point is fenced in by
    // its neighbours rather than allowed to cross them.
    const previous = dragIndex > 0 ? points[dragIndex - 1][0] + MIN_GAP_S : 0;
    const next =
      dragIndex < points.length - 1 ? points[dragIndex + 1][0] - MIN_GAP_S : Number.POSITIVE_INFINITY;

    const time = Math.min(Math.max(snapTime(position.time), previous), Math.min(next, span));
    onChange(
      points.map((point, index) =>
        index === dragIndex ? ([time, snapValue(position.value)] as ProfilePoint) : point,
      ),
    );
  };

  const addPointAt = (event: React.MouseEvent) => {
    if (dragIndex !== undefined) return;
    const position = fromEvent(event);
    if (!position) return;
    const time = snapTime(position.time);
    // Refuse a point that would sit on top of an existing one: it would break
    // the strictly-increasing rule the solver validates.
    if (points.some((point) => Math.abs(point[0] - time) < MIN_GAP_S)) return;

    const next: ProfilePoint[] = [...points, [time, snapValue(position.value)]];
    next.sort((a, b) => a[0] - b[0]);
    onChange(next);
  };

  const removePoint = (index: number) => {
    if (points.length <= 1) return;
    onChange(points.filter((_, i) => i !== index));
  };

  const movePoint = (index: number, timeChange: number, valueChange: number) => {
    const point = points[index];
    if (!point) return;
    const previous = index > 0 ? points[index - 1][0] + MIN_GAP_S : 0;
    const next = index < points.length - 1 ? points[index + 1][0] - MIN_GAP_S : span;
    const time = Math.min(Math.max(snapTime(point[0] + timeChange), previous), next);
    const value = snapValue(point[1] + valueChange);
    onChange(points.map((current, currentIndex) =>
      currentIndex === index ? ([time, value] as ProfilePoint) : current,
    ));
  };

  const handlePointKeyDown = (event: React.KeyboardEvent<SVGCircleElement>, index: number) => {
    switch (event.key) {
      case "ArrowLeft":
        event.preventDefault();
        movePoint(index, -TIME_SNAP, 0);
        break;
      case "ArrowRight":
        event.preventDefault();
        movePoint(index, TIME_SNAP, 0);
        break;
      case "ArrowDown":
        event.preventDefault();
        movePoint(index, 0, -0.1);
        break;
      case "ArrowUp":
        event.preventDefault();
        movePoint(index, 0, 0.1);
        break;
    }
  };

  const path = points.length
    ? [
        `M ${toX(0)} ${toY(points[0][1])}`,
        ...points.map((point) => `L ${toX(point[0])} ${toY(point[1])}`),
        // Held flat after the last point, exactly as the solver samples it.
        `L ${toX(span)} ${toY(points[points.length - 1][1])}`,
      ].join(" ")
    : "";

  const ticks = [low, low + (high - low) / 2, high];

  return (
    <div>
      <svg
        ref={svgRef}
        viewBox={`0 0 ${VIEW.width} ${VIEW.height}`}
        style={{ width: "100%", height: "auto", touchAction: "none", cursor: "crosshair" }}
        role="group"
        aria-label={`${unit} profile editor`}
        aria-describedby={instructionsId}
        onPointerMove={handleMove}
        onPointerUp={() => setDragIndex(undefined)}
        onPointerLeave={() => setDragIndex(undefined)}
        onDoubleClick={addPointAt}
      >
        <rect
          x={PAD.left} y={PAD.top} width={PLOT_WIDTH} height={PLOT_HEIGHT}
          fill="var(--bg)" stroke="var(--line)" strokeWidth={0.5}
        />
        {ticks.map((tick) => (
          <g key={tick}>
            <line
              x1={PAD.left} x2={PAD.left + PLOT_WIDTH} y1={toY(tick)} y2={toY(tick)}
              stroke="var(--line)" strokeWidth={0.5} strokeDasharray="2 3"
            />
            <text x={PAD.left - 4} y={toY(tick) + 3} textAnchor="end" fontSize={7}
                  fill="var(--muted)">
              {Number.isInteger(tick) ? tick : tick.toFixed(1)}
            </text>
          </g>
        ))}
        {[0, span / 2, span].map((time) => (
          <text key={time} x={toX(time)} y={VIEW.height - 7} textAnchor="middle" fontSize={7}
                fill="var(--muted)">
            {time.toFixed(0)}
          </text>
        ))}
        <text x={PAD.left + PLOT_WIDTH} y={VIEW.height - 7} textAnchor="end" fontSize={7}
              fill="var(--muted)">
          s
        </text>
        <text x={2} y={PAD.top + 6} fontSize={7} fill="var(--muted)">
          {unit}
        </text>

        <path d={path} fill="none" stroke={color} strokeWidth={1.6} strokeLinejoin="round" />

        {points.map((point, index) => (
          <circle
            key={index}
            cx={toX(point[0])}
            cy={toY(point[1])}
            r={dragIndex === index || hoverIndex === index ? 4.5 : 3.2}
            fill={color}
            stroke="var(--panel)"
            strokeWidth={1.2}
            style={{ cursor: "grab" }}
            role="slider"
            tabIndex={0}
            aria-label={`Profile point ${index + 1}`}
            aria-valuemin={low}
            aria-valuemax={high}
            aria-valuenow={point[1]}
            aria-valuetext={`${point[0]} seconds, ${point[1]} ${unit}`}
            onPointerDown={(event) => {
              event.stopPropagation();
              (event.target as Element).setPointerCapture?.(event.pointerId);
              setDragIndex(index);
            }}
            onPointerEnter={() => setHoverIndex(index)}
            onPointerLeave={() => setHoverIndex(undefined)}
            onKeyDown={(event) => handlePointKeyDown(event, index)}
            onDoubleClick={(event) => {
              event.stopPropagation();
              removePoint(index);
            }}
          />
        ))}

        {hoverIndex !== undefined && points[hoverIndex] && (
          <text
            x={Math.min(toX(points[hoverIndex][0]) + 6, VIEW.width - 40)}
            y={Math.max(toY(points[hoverIndex][1]) - 6, PAD.top + 8)}
            fontSize={7.5}
            fill="var(--text)"
            style={{ fontFamily: "ui-monospace, Menlo, monospace" }}
          >
            {points[hoverIndex][0]}s · {points[hoverIndex][1]}
          </text>
        )}
      </svg>
      <p id={instructionsId} className="note" style={{ marginTop: 2 }}>
        Drag a point to move it. Double-click the plot to add and a point to remove. Focus a point
        and use arrow keys to adjust its time or value.
      </p>
    </div>
  );
}
