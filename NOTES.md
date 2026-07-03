# NOTES

Deviations, justifications, and spot-check evidence, per PLAN.md §1 rule 7 and §13.7.
Never weaken a gate to make it pass (§1 rule 2); if a spec is contradictory or impossible,
document it here and report instead of changing constants or tests.

## M0 — Scaffold and toolchain

- Resolved exact GitHub repositories/tags for the §4 pins by web search (PLAN.md §4 gives
  library names and version numbers but not org/repo paths):
  - libebur128 → `jiixyj/libebur128`, tag `v1.2.6` (matches the plan's known-good pin exactly).
  - KissFFT → `mborgerding/kissfft`, tag `131.1.0` (matches exactly).
  - nlohmann/json → tag `v3.11.3` (latest `v3.11.x`).
  - Dear ImGui docking → tag `v1.92.8-docking` (latest docking tag; satisfies `>= 1.90.5` and
    includes `AddConcavePolyFilled`).
  - GLFW → tag `3.4` (matches exactly).
  - nativefiledialog-extended → tag `v1.2.1` (latest `v1.2.x`).
  - doctest → tag `v2.4.12` (latest `v2.4.x`).
- Added one dependency beyond the §4 table: `extern/sha256` (Brad Conte's public-domain
  `crypto-algorithms` SHA-256, single .c/.h pair). This is explicitly pre-authorized by
  PLAN.md §12/M3 ("vendor a small public-domain implementation or use a header-only one;
  record in DEPENDENCIES.md"), so it is not a rule-7 violation, but recorded here for
  traceability. Pinned to commit `cfbde48414baacf51fc7c74f275190881f037d32` (repo carries no
  tags).
- `app/` and `cli/`/`tools/genfixtures` contain M0 placeholder `main.cpp` stubs (exit code 2,
  "not implemented yet") so the top-level build is exercised end-to-end from M0 onward without
  pulling in GLFW/ImGui/NFD before M6 needs them. `core/` similarly holds only `version.h` for
  now; the rest of the public API (`audio.h`, `features.h`, `mapping.h`, `manifest.h`,
  `glyph.h`, `lint.h`) lands in M1–M4 as specified.
- `cmake/Dependencies.cmake` declares `FetchContent` sources for every non-vendored pin up
  front (a single registry of truth for versions), but only `doctest` is made available at M0
  — the others are pulled in by the milestone that first needs them, per §1 rule 6 (layering)
  and to keep each milestone's build/gate fast and scoped.
