# Documentation Expansion Design

## Purpose

Make EspressoLab easier to operate and contribute to while correcting documented
claims that have drifted from the implementation. The documentation must make a
clear distinction between implemented behavior, local verification, and
real-world validation.

## Audience

The documentation serves both users and contributors:

- Users need a reproducible route from clone to build, simulation, dashboard,
  artifacts, sweeps, and the separate CFD command.
- Contributors need clear module ownership, dependency boundaries, quality
  gates, and a deliberate process for changing data contracts.

## Structure

### README

Keep the README as the concise project entry point. It will retain the product
overview, quick start, architecture sketch, limitations, and command summary.
It will link to the detailed guides rather than duplicating their instructions.

### Getting Started Guide

Add `docs/getting-started.md` for first-time users. It will cover prerequisites,
native build and test commands, dashboard startup, a first CLI simulation,
artifact locations, and troubleshooting boundaries for the local-only server.

### Development Guide

Add `docs/development.md` for contributors. It will document the source layout,
module ownership, inward dependency rule, build variants, test commands, and
the required workflow for edits that cross loader, schema, serializer, REST,
or TypeScript boundaries.

### Data Contract Guide

Add `docs/data-contracts.md` to define ownership and lifecycle of recipe,
coefficient, result, sweep, and reference-shot data. It will identify the
runtime loader and serializer as the current executable contract, describe
units and versioning, and require schemas, API documentation, and web types to
be updated and tested together when a contract changes.

### Existing Documents

Update architecture, API, model, testing, and current-state documents where
their topics belong. Remove draft prose and stale claims. Documentation will
describe the implemented CFD momentum closure as Darcy and list Forchheimer
inertia as future work until it is implemented.

## Accuracy Rules

- Do not claim that a configured, unimplemented, or untested capability works.
- Describe the Level 1-3 and CFD solvers as verified only to the extent covered
  by tests; neither is validated against real espresso measurements.
- Identify the local REST server as development tooling, not a hosted service.
- Keep command examples consistent with the current scripts and executable
  names.

## Verification

The documentation change will be checked by validating internal Markdown links,
reviewing documented commands against scripts and build targets, and running the
existing native and web type/build checks where available. It does not include
physics, API, or frontend behavior fixes found by the code review.

## Scope Boundaries

This change does not refactor production code, implement CFD inertia, alter the
public API, or add a documentation-site generator. Contract discrepancies will
be documented as maintenance responsibilities without papering over the
implementation defects identified in the review.
