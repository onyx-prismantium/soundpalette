# CLAUDE.md

SoundPalette is a cross-platform desktop tool that treats a game's sound effects like a color
palette: it scans a folder of audio files, extracts perceptual audio features (brightness,
attack, decay tail, noisiness, warmth, loudness, grit), maps them through a fixed, versioned
mapping to visual attributes (hue, lightness, saturation, size, spikiness, edge jitter, tail
length), and renders every sound as a glyph in a grid — so a cohesive sound identity looks like
a cohesive palette, and off-brand sounds are visible (and lintable) at a glance. It ships as one
C++ codebase producing three artifacts: `libsoundpalette` (headless core), the `soundpalette`
CLI (`scan`/`lint`/`export-svg`/`watch`/`print-mapping`, usable as a CI "palette lint" gate), and
`soundpalette-app` (a Dear ImGui desktop shell with a live mapping tuner).

Read `PLAN.md` for all specs — it is the normative source for the DSP formulas (§6), the mapping
constants (§7), the manifest schema (§8), the CLI/GUI behavior (§9–10), the fixtures/tolerances
(§11), and the milestone gates (§12). **Never weaken a gate to make it pass**: numeric
tolerances are part of the spec; if something looks contradictory or impossible, stop and
document it in `NOTES.md` instead of quietly changing a constant or a test.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Test

Fixtures are generated, not committed (deterministic — see PLAN.md §11):

```bash
./build/tools/genfixtures/genfixtures tests/golden/fixtures
bash tests/integration/make_lossy.sh tests/golden/fixtures
```

Then:

```bash
ctest --test-dir build --output-on-failure
for s in tests/integration/*.sh; do bash "$s" || echo "FAILED: $s"; done
```

Targeted subsets used by milestone gates: `ctest --test-dir build -R "decode|loudness"`,
`-R features`, `-R "mapping|manifest"`, `-R lint`.

## GUI smoke test (headless)

```bash
cmake --build build --target soundpalette-app
xvfb-run -a ./build/app/soundpalette-app --smoke /tmp/smoke.png --dir tests/golden
```

## Layout

`core/` = headless engine (no GLFW/OpenGL/ImGui/NFD includes, ever — verify with
`grep -rE "imgui|GLFW|GL/" core/`). `cli/` links core only. `app/` links core + UI libs.
`cmake/Dependencies.cmake` is the pin registry; `DEPENDENCIES.md` is the human-readable version
with licenses. Milestone status and any deviations live in `NOTES.md`.
