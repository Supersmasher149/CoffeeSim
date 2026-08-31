import { render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it } from "vitest";

import { makeReferenceCatalogue, makeReferenceRecord } from "../../test/fixtures/reference";
import { makeShotResult } from "../../test/fixtures/shotResult";
import { makeRecipe } from "../../test/fixtures/recipe";
import { ReferenceShotsPanel } from "./ReferenceShotsPanel";

describe("ReferenceShotsPanel: loading and error states", () => {
  it("shows a loading message before the catalogue arrives", () => {
    render(<ReferenceShotsPanel />);
    expect(screen.getByText(/loading reference catalogue/i)).toBeInTheDocument();
  });

  it("shows the error message and never a stale loading message", () => {
    render(<ReferenceShotsPanel error="network unreachable" />);
    expect(screen.getByText(/reference catalogue unavailable: network unreachable/i)).toBeInTheDocument();
    expect(screen.queryByText(/loading reference catalogue/i)).not.toBeInTheDocument();
  });

  it("shows an empty-catalogue message when there are no references", () => {
    render(<ReferenceShotsPanel catalogue={makeReferenceCatalogue({ references: [] })} />);
    expect(screen.getByText(/no valid reference records are available/i)).toBeInTheDocument();
  });

  it("lists partial load errors alongside any records that did load", () => {
    render(
      <ReferenceShotsPanel
        catalogue={makeReferenceCatalogue({
          load_errors: [{ file: "broken.json", code: "SCHEMA_VIOLATION", message: "missing setup.machine" }],
        })}
      />,
    );
    expect(screen.getByText("SCHEMA_VIOLATION")).toBeInTheDocument();
    expect(screen.getByText(/broken\.json: missing setup\.machine/)).toBeInTheDocument();
    // The one good record still renders alongside the load error.
    expect(screen.getByRole("table")).toBeInTheDocument();
  });
});

describe("ReferenceShotsPanel: null/optional observed fields", () => {
  it("renders 'not reported' for a null observed field instead of a NaN or blank cell", () => {
    const catalogue = makeReferenceCatalogue({
      references: [
        makeReferenceRecord({
          observed: {
            ...makeReferenceRecord().observed,
            final_shot_time_s: null,
          },
        }),
      ],
    });
    render(<ReferenceShotsPanel catalogue={catalogue} />);
    expect(screen.getByText("not reported")).toBeInTheDocument();
  });
});

describe("ReferenceShotsPanel: current-model association", () => {
  it("omits the current-model column entirely when there is no active run", () => {
    render(<ReferenceShotsPanel catalogue={makeReferenceCatalogue()} />);
    expect(screen.queryByText("Current model")).not.toBeInTheDocument();
  });

  it("reads dose from the given recipe and every other current-model metric from the active result", () => {
    const recipe = makeRecipe({ puck: { ...makeRecipe().puck, dose_g: 18.4 } });
    const active = makeShotResult({ elapsed_time_s: 27.5, tds_percent: 9.44, brew_ratio: 2.1 });
    render(<ReferenceShotsPanel catalogue={makeReferenceCatalogue()} active={active} recipe={recipe} />);

    const table = screen.getByRole("table");
    const doseRow = within(table).getAllByRole("row").find((row) => /dose/i.test(row.textContent ?? ""))!;
    expect(within(doseRow).getByText("18.4 g", { selector: ".current-model" })).toBeInTheDocument();

    const shotTimeRow = within(table).getAllByRole("row").find((row) => /shot time/i.test(row.textContent ?? ""))!;
    expect(within(shotTimeRow).getByText("27.5 s", { selector: ".current-model" })).toBeInTheDocument();
  });

  it("shows 'not reported' for dose when active is set but recipe is undefined", () => {
    const active = makeShotResult();
    render(<ReferenceShotsPanel catalogue={makeReferenceCatalogue()} active={active} recipe={undefined} />);
    const table = screen.getByRole("table");
    const doseRow = within(table).getAllByRole("row").find((row) => /dose/i.test(row.textContent ?? ""))!;
    expect(within(doseRow).getByText("not reported", { selector: ".current-model" })).toBeInTheDocument();
  });
});

describe("ReferenceShotsPanel: external link safety", () => {
  it("opens source links in a new tab with rel=noreferrer", () => {
    render(<ReferenceShotsPanel catalogue={makeReferenceCatalogue()} />);
    const articleLink = screen.getByRole("link", { name: "article" });
    expect(articleLink).toHaveAttribute("target", "_blank");
    expect(articleLink).toHaveAttribute("rel", "noreferrer");
    const logLink = screen.getByRole("link", { name: "experiment log" });
    expect(logLink).toHaveAttribute("target", "_blank");
    expect(logLink).toHaveAttribute("rel", "noreferrer");
  });
});

describe("ReferenceShotsPanel: setup disclosures", () => {
  it("keeps setup details collapsed until the disclosure is opened", async () => {
    // jsdom does not implement the native <details> closed-content hiding
    // (that's a rendering/layout behaviour, not a DOM-presence one), so the
    // meaningful assertion here is the element's own `open` state rather
    // than whether its content is queryable -- visual collapse is
    // Playwright's job (web/e2e/).
    const user = userEvent.setup();
    render(<ReferenceShotsPanel catalogue={makeReferenceCatalogue()} />);
    const summary = screen.getByText(/GUJI/i);
    const details = summary.closest("details")!;
    expect(details.open).toBe(false);
    await user.click(summary);
    expect(details.open).toBe(true);
    expect(screen.getByText("Bloom")).toBeInTheDocument();
  });
});
