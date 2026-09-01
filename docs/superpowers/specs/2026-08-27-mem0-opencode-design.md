# Mem0 OpenCode Integration Design

## Goal

Give OpenCode persistent, cloud-backed memory for the EspressoLab project while
keeping memories scoped to this repository and keeping credentials out of the
worktree.

## Scope

This integration is for OpenCode only. It does not add Mem0 to the EspressoLab
native solver, REST server, dashboard, build graph, or web dependencies.

## Architecture

Use the official `@mem0/opencode-plugin` as a project-level OpenCode plugin.
Installing it without `--global` adds the plugin to the repository's
`opencode.json`. The plugin provides native Mem0 tools, lifecycle hooks, and
Mem0 skills through the `mem0ai` SDK; a separate MCP server is not needed.

Remove the existing legacy global `@mem0/mcp-server` entry from
`~/.config/opencode/opencode.json` to prevent duplicate Mem0 registrations.
That user-level edit is local machine configuration and is not committed to the
repository.

## Credentials And Scope

`MEM0_API_KEY` must be provided through the user's shell environment, preferably
in `~/.zshrc` on this macOS development machine. The key must never be written
to `opencode.json`, a project file, command history, or version control.

The plugin's default `project` scope is retained. OpenCode must be launched
from the repository so Mem0 derives the project identity from the EspressoLab
git worktree. Global memory scope is not enabled by default.

## Verification

After installation and an OpenCode restart:

1. Confirm the key is present without printing its value.
2. Run `/mem0:health` and confirm cloud connectivity.
3. Store a harmless project-specific test memory with `/mem0:remember`.
4. Retrieve it with `/mem0:peek` and confirm it is associated with EspressoLab.
5. Confirm only one Mem0 integration is active.

No native or web test suite changes are required. If the cloud key is absent or
invalid, installation remains safe but verification must stop at the credential
error rather than placing a fallback secret in the repository.
