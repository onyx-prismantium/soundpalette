# Contributing to SoundPalette

Thanks for your interest! A few ground rules keep this codebase healthy:

## Before you start

- `PLAN.md` is the **normative spec** — DSP formulas, mapping constants, manifest schema,
  tolerances, and milestone gates all live there. Behavior changes need a spec change first.
- **Never weaken a gate to make it pass.** Numeric tolerances are part of the spec. If
  something looks contradictory or impossible, open an issue instead of changing a constant.
- Deviations and their rationale are logged in `NOTES.md`; read it before assuming a bug.

## Building and testing

Build and test instructions are in [README.md](README.md#quickstart-ubuntu-22042404) and
`CLAUDE.md`. A pull request is expected to be green on:

```bash
ctest --test-dir build --output-on-failure          # unit tests
for s in tests/integration/*.sh; do bash "$s"; done # integration scripts
bash tests/integration/mcp_smoke.sh                 # MCP server (needs node >= 18)
```

plus `clang-format` clean (`.clang-format` at the repo root is authoritative).

## Layering rules

- `core/` is headless: no GLFW/OpenGL/ImGui/NFD includes, ever.
- `cli/` links core only. `app/` links core + UI libraries.
- Dependency pins live in `cmake/Dependencies.cmake` and are documented in `DEPENDENCIES.md`.

## Submitting changes

- One logical change per PR, with tests that demonstrate it.
- Golden manifests are regenerated deliberately, never hand-edited — say so in the PR when
  you regenerate them, and why.
- By submitting a contribution you agree it is licensed under the repository's
  [Apache-2.0 license](LICENSE).
