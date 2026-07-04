# SoundPalette

SoundPalette treats a game's sound effects the way art direction treats color: **as a palette**.

It scans a folder of audio files (`.wav` `.flac` `.ogg` `.mp3`), extracts perceptual audio
features — brightness, attack, decay tail, noisiness, warmth, loudness, grit — and maps them
through a fixed, versioned mapping to visual attributes: hue, lightness, saturation, size,
spikiness, edge jitter, tail length. Every sound becomes a glyph in a grid. A cohesive sound
identity shows up as a cohesive palette; off-brand sounds are visible at a glance — and can be
flagged automatically in CI.

![SoundPalette app](docs/screenshot.png)

One C++20 codebase, three artifacts:

| Artifact | What it is |
|---|---|
| `libsoundpalette` | Headless static library: decode → normalize → features → mapping → manifest |
| `soundpalette` | CLI: `scan`, `lint`, `export-svg`, `watch`, `print-mapping` — CI-friendly, deterministic output |
| `soundpalette-app` | Desktop shell (Dear ImGui + GLFW + OpenGL 3): glyph grid, click-to-play, hover details, live mapping tuner |

## Quickstart (Ubuntu 22.04/24.04)

```bash
sudo apt-get update && sudo apt-get install -y \
  build-essential cmake ninja-build git pkg-config \
  xorg-dev libwayland-dev libxkbcommon-dev wayland-protocols \
  libgl1-mesa-dev \
  libasound2-dev libpulse-dev \
  libgtk-3-dev libdbus-1-dev \
  ffmpeg libxml2-utils xvfb

git clone <this-repo> soundpalette && cd soundpalette
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Then point it at a folder of sounds:

```bash
# Analyze a folder -> canonical JSON manifest
./build/cli/soundpalette scan path/to/sfx --out palette.json

# Render the palette as an SVG sheet
./build/cli/soundpalette export-svg palette.json --out sheet.svg --columns 8

# Open the GUI
./build/app/soundpalette-app
```

## CLI

```
soundpalette scan <dir> [--out palette.json] [--no-meta] [--threads N] [--quiet]
soundpalette lint <dir> --baseline palette.json [--threshold 2.5] [--top 10]
soundpalette export-svg <palette.json> --out sheet.svg [--columns 8]
soundpalette watch <dir> [--out palette.json] [--interval-ms 500]
soundpalette print-mapping [--out mapping.json]
```

Exit codes: `0` success · `1` lint outliers found · `2` usage/IO errors.

Manifests are **deterministic**: same input folder ⇒ byte-identical JSON (sorted entries,
rounded floats, no timestamps, seeded glyph randomness). Two `export-svg` runs of the same
manifest produce identical bytes.

## Palette linting in CI

Commit a baseline manifest for your approved sound set, then fail the build when a new sound
drifts off-palette:

```bash
# Once, on a sound set you're happy with:
soundpalette scan sfx/ --out sfx/palette-baseline.json
git add sfx/palette-baseline.json

# In CI:
soundpalette lint sfx/ --baseline sfx/palette-baseline.json --threshold 2.5
# exit 1 + report if any file is a statistical outlier in mapping space:
#   OUTLIER ui/laser_zap.wav worst=bright01 z=+5.12 dims=bright01,ton01
```

Lint compares each file against the baseline's stats in the seven-dimensional mapping space
`[bright01, warm01, ton01, atk01, tail01, loud01, jitter01]` (z-score per dimension, std
floored at 0.02). Tune `--threshold` to your set's natural spread: tight, homogeneous packs
can afford 2.5; small or heterogeneous sets may need 3–4 (see `tests/integration/lint_outlier.sh`
for a worked example).

## GUI

`soundpalette-app` shows the palette live: sort by hue/brightness/size/attack/tail/name,
filter by name, click a glyph to hear it, hover for its feature breakdown, and drag the
**mapping tuner** sliders to re-derive the entire grid's visuals in real time (no re-analysis).
Export the tuned mapping as JSON.

Headless self-test (used by CI):

```bash
xvfb-run -a ./build/app/soundpalette-app --smoke out.png --dir path/to/sfx
```

## Using SoundPalette from an AI agent

`mcp/` ships an MCP server (stdio transport) that exposes the analysis suite as tools:
`scan_folder`, `lint_against_baseline`, `describe_sound` (deterministic plain-language
description), and `render_palette_sheet` (returns the glyph grid as a PNG image). Every path
is confined to the configured project root.

```bash
cd mcp && npm ci && npm run build
```

Then register it with any MCP client (see https://modelcontextprotocol.io for your client's
registration format):

```json
{
  "command": "node",
  "args": ["<repo>/mcp/dist/server.js", "--root", "<your-sound-project>"]
}
```

Add `--bin <path-to-soundpalette>` if the CLI is not at `<repo>/build/cli/soundpalette`.

## Development

- `PLAN.md` — normative spec: DSP formulas (§6), mapping constants (§7), manifest schema (§8),
  CLI/GUI behavior (§9–10), fixtures and tolerances (§11), milestone gates (§12).
- `NOTES.md` — deviation log, golden-manifest spot-checks, manual GUI checklist.
- `DEPENDENCIES.md` / `LICENSES.md` — pinned versions and licenses.

```bash
# Regenerate deterministic test fixtures, then run everything
./build/tools/genfixtures/genfixtures tests/golden/fixtures
bash tests/integration/make_lossy.sh tests/golden/fixtures
ctest --test-dir build --output-on-failure
for s in tests/integration/*.sh; do bash "$s" || exit 1; done
```

Layering is enforced: `core/` never includes GLFW/OpenGL/ImGui/NFD; the CLI links core only;
the app links core + UI libraries.

## License

The SoundPalette source is provided under the MIT license. Third-party components are listed
in [LICENSES.md](LICENSES.md); all are permissive (MIT/BSD/zlib/public-domain) and statically
linked.
