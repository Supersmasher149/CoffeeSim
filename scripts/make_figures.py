#!/usr/bin/env python3
"""Regenerate the README figures.

The sweep chart is drawn from a real sweep artifact, not by hand: every point on
it is traceable to a run in outputs/sweeps/grind-size/aggregate.csv. Re-run
`scripts/demo.sh` first if that file is missing or stale.

Light and dark variants are emitted for each figure. GitHub serves README
images as <img>, so a standalone SVG cannot inherit the page's foreground with
currentColor; the README selects between the two with <picture>.
"""

import csv
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SWEEP = ROOT / "outputs" / "sweeps" / "grind-size" / "aggregate.csv"
IMAGES = ROOT / "docs" / "images"

# Both pairs pass the six checks of the dataviz palette validator against their
# own surface: lightness band, chroma floor, CVD separation, normal-vision
# separation and contrast.
THEMES = {
    "light": {
        "surface": "#ffffff",
        "ink": "#1f2328",
        "muted": "#59636e",
        "grid": "#d8dee4",
        "line": "#8c959f",
        "series": ["#b5651f", "#1069c9"],
        "box": "#f6f8fa",
    },
    "dark": {
        "surface": "#0d1117",
        "ink": "#e6edf3",
        "muted": "#9198a1",
        "grid": "#2a313c",
        "line": "#6a737d",
        "series": ["#c47733", "#3f97cc"],
        "box": "#161b22",
    },
}

FONT = "ui-monospace, SFMono-Regular, Menlo, monospace"
SANS = "-apple-system, BlinkMacSystemFont, Segoe UI, Helvetica, Arial, sans-serif"


def esc(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# --------------------------------------------------------------------------
# Figure 1: architecture


def architecture(theme):
    t = THEMES[theme]
    W, H = 780, 286
    out = []

    def box(x, y, w, h, label, sub=None, emphasis=False):
        stroke = t["series"][0] if emphasis else t["line"]
        width = 1.6 if emphasis else 1.1
        out.append(
            f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="5" '
            f'fill="{t["box"]}" stroke="{stroke}" stroke-width="{width}"/>'
        )
        ty = y + (h / 2 + 4 if sub is None else h / 2 - 3)
        out.append(
            f'<text x="{x + w / 2}" y="{ty}" text-anchor="middle" font-family="{FONT}" '
            f'font-size="12" fill="{t["ink"]}">{esc(label)}</text>'
        )
        if sub:
            out.append(
                f'<text x="{x + w / 2}" y="{y + h / 2 + 12}" text-anchor="middle" '
                f'font-family="{SANS}" font-size="10" fill="{t["muted"]}">{esc(sub)}</text>'
            )

    def arrow(x1, y1, x2, y2, label=None, dy=-6, anchor="middle"):
        out.append(
            f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{t["line"]}" '
            f'stroke-width="1.3" marker-end="url(#a-{theme})"/>'
        )
        if label:
            out.append(
                f'<text x="{(x1 + x2) / 2}" y="{(y1 + y2) / 2 + dy}" text-anchor="{anchor}" '
                f'font-family="{SANS}" font-size="10" fill="{t["muted"]}">{esc(label)}</text>'
            )

    out.append(
        f'<defs><marker id="a-{theme}" viewBox="0 0 10 10" refX="9" refY="5" '
        f'markerWidth="6" markerHeight="6" orient="auto-start-reverse">'
        f'<path d="M 0 0 L 10 5 L 0 10 z" fill="{t["line"]}"/></marker></defs>'
    )

    box(12, 100, 122, 44, "web", "React / TS")
    arrow(136, 122, 182, 122, "HTTP", dy=-6)

    box(184, 32, 150, 40, "tool_server", None)
    box(184, 102, 150, 40, "espressolab_cli", None)
    box(184, 172, 150, 40, "tests", None)

    # Three drivers, one entry point: this is what the dependency rule buys.
    for y in (52, 122, 192):
        arrow(336, y, 434, 122)
    # Two balanced lines so the signature fits the gap between the drivers and
    # the core instead of running under either box.
    out.append(
        f'<text x="385" y="100" text-anchor="middle" font-family="{SANS}" font-size="10" '
        f'fill="{t["muted"]}">run(recipe,</text>'
    )
    out.append(
        f'<text x="385" y="112" text-anchor="middle" font-family="{SANS}" font-size="10" '
        f'fill="{t["muted"]}">coefficients)</text>'
    )

    box(436, 96, 158, 52, "espresso_core", "state · stepping · termination", emphasis=True)
    arrow(515, 150, 515, 190, None)
    out.append(
        f'<text x="523" y="174" font-family="{SANS}" font-size="10" '
        f'fill="{t["muted"]}">correlations</text>'
    )
    box(436, 192, 158, 48, "model_library", "water · puck · extraction")

    arrow(596, 122, 632, 122, None)
    out.append(
        f'<text x="614" y="112" text-anchor="middle" font-family="{SANS}" font-size="10" '
        f'fill="{t["muted"]}">result</text>'
    )
    box(634, 96, 130, 52, "artifact_io", "JSON · CSV · hash")

    out.append(
        f'<text x="390" y="268" text-anchor="middle" font-family="{SANS}" font-size="11" '
        f'fill="{t["muted"]}">Every arrow points inward. The core has no dependency on '
        f'HTTP, JSON or the browser.</text>'
    )

    label = ("EspressoLab architecture: the CLI, the REST server and the tests all drive one "
             "simulation core, which depends on the model library and knows nothing about "
             "HTTP, JSON or the browser.")
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" '
        f'height="{H}" role="img" aria-label="{esc(label)}">'
        f"<title>{esc(label)}</title>"
        f'<rect width="{W}" height="{H}" fill="{t["surface"]}"/>' + "".join(out) + "</svg>"
    )


# --------------------------------------------------------------------------
# Figure 2: grind-size sweep


def read_sweep():
    if not SWEEP.exists():
        sys.exit(f"missing {SWEEP} - run scripts/demo.sh first")
    with SWEEP.open() as handle:
        rows = list(csv.DictReader(handle))
    return [
        {
            "grind": float(r["puck.particle_diameter_um"]),
            "time": float(r["shot_time_s"]),
            "tds": float(r["tds_percent"]),
            "yield": float(r["extraction_yield_percent"]),
            "stopped_on_time": r["termination"] == "time_limit_reached",
        }
        for r in rows
    ]


def sweep_chart(theme, rows):
    t = THEMES[theme]
    W, H = 720, 452
    L, R = 62, 132        # right margin holds the direct labels
    panels = [
        {"y": 44, "h": 126, "title": "Shot time to 36 g", "unit": "s",
         "series": [("time", "shot time", 0)]},
        {"y": 248, "h": 126, "title": "Strength and extraction", "unit": "%",
         "series": [("yield", "extraction yield", 0), ("tds", "TDS", 1)]},
    ]
    xs = [r["grind"] for r in rows]
    x_min, x_max = min(xs), max(xs)
    out = []

    def to_x(v):
        return L + (v - x_min) / (x_max - x_min) * (W - L - R)

    for panel in panels:
        values = [r[key] for key, _, _ in panel["series"] for r in rows]
        lo, hi = min(values), max(values)
        pad = (hi - lo) * 0.18 or 1
        lo, hi = max(0, lo - pad), hi + pad

        def to_y(v, p=panel, lo=lo, hi=hi):
            return p["y"] + p["h"] - (v - lo) / (hi - lo) * p["h"]

        out.append(
            f'<text x="{L}" y="{panel["y"] - 14}" font-family="{SANS}" font-size="13" '
            f'font-weight="600" fill="{t["ink"]}">{esc(panel["title"])} '
            f'<tspan fill="{t["muted"]}" font-weight="400">({panel["unit"]})</tspan></text>'
        )

        for step in range(4):
            value = lo + (hi - lo) * step / 3
            y = to_y(value)
            out.append(
                f'<line x1="{L}" y1="{y:.1f}" x2="{W - R}" y2="{y:.1f}" '
                f'stroke="{t["grid"]}" stroke-width="1"/>'
            )
            out.append(
                f'<text x="{L - 8}" y="{y + 3.5:.1f}" text-anchor="end" font-family="{FONT}" '
                f'font-size="10" fill="{t["muted"]}">{value:.0f}</text>'
            )

        for key, name, slot in panel["series"]:
            colour = t["series"][slot]
            points = " ".join(f"{to_x(r['grind']):.1f},{to_y(r[key]):.1f}" for r in rows)
            out.append(
                f'<polyline points="{points}" fill="none" stroke="{colour}" '
                f'stroke-width="2" stroke-linejoin="round"/>'
            )
            for r in rows:
                # A hollow marker flags the run that never reached its target
                # mass, so the reader is not told a stalled shot is a data point
                # like the others.
                hollow = r["stopped_on_time"]
                out.append(
                    f'<circle cx="{to_x(r["grind"]):.1f}" cy="{to_y(r[key]):.1f}" r="4" '
                    f'fill="{t["surface"] if hollow else colour}" stroke="{colour}" '
                    f'stroke-width="2"/>'
                )
            last = rows[-1]
            out.append(
                f'<text x="{W - R + 10}" y="{to_y(last[key]) + 4:.1f}" font-family="{SANS}" '
                f'font-size="11" fill="{t["ink"]}">{esc(name)}</text>'
            )
            out.append(
                f'<circle cx="{W - R + 3}" cy="{to_y(last[key]):.1f}" r="3.5" fill="{colour}"/>'
            )

        out.append(
            f'<line x1="{L}" y1="{panel["y"] + panel["h"]}" x2="{W - R}" '
            f'y2="{panel["y"] + panel["h"]}" stroke="{t["line"]}" stroke-width="1"/>'
        )
        for r in rows[::2]:
            x = to_x(r["grind"])
            out.append(
                f'<text x="{x:.1f}" y="{panel["y"] + panel["h"] + 16}" text-anchor="middle" '
                f'font-family="{FONT}" font-size="10" fill="{t["muted"]}">{r["grind"]:.0f}</text>'
            )

    # Anchored below the last panel's tick row rather than to the canvas height,
    # so changing a panel size cannot slide these back into the plot.
    baseline = panels[-1]["y"] + panels[-1]["h"] + 40
    out.append(
        f'<text x="{(L + W - R) / 2}" y="{baseline}" text-anchor="middle" font-family="{SANS}" '
        f'font-size="11" fill="{t["muted"]}">representative particle diameter (µm)</text>'
    )
    stalled = [r for r in rows if r["stopped_on_time"]]
    if stalled:
        out.append(
            f'<circle cx="{L + 4}" cy="{baseline + 20}" r="4" fill="{t["surface"]}" '
            f'stroke="{t["series"][0]}" stroke-width="2"/>'
        )
        out.append(
            f'<text x="{L + 16}" y="{baseline + 24}" font-family="{SANS}" font-size="11" '
            f'fill="{t["muted"]}">hollow: hit the 45 s limit before reaching 36 g '
            f'(not a completed shot)</text>'
        )

    label = ("Grind-size sweep: finer grind lengthens the shot and raises strength and "
             "extraction, until the finest puck stalls at the time limit.")
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" '
        f'height="{H}" role="img" aria-label="{esc(label)}">'
        f"<title>{esc(label)}</title>"
        f'<rect width="{W}" height="{H}" fill="{t["surface"]}"/>' + "".join(out) + "</svg>"
    )


def main():
    IMAGES.mkdir(parents=True, exist_ok=True)
    rows = read_sweep()
    for theme in THEMES:
        (IMAGES / f"architecture-{theme}.svg").write_text(architecture(theme))
        (IMAGES / f"grind-sweep-{theme}.svg").write_text(sweep_chart(theme, rows))
    print(f"wrote 4 figures to {IMAGES} from {len(rows)} sweep runs")


if __name__ == "__main__":
    main()
