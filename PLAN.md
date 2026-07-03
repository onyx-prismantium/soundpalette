# SoundPalette — Implementation Plan v1 (Linux-first)

An executable build plan for an agentic developer (Claude Code). Work through it top to bottom.
Sections 0–2 are context and setup, 3–11 are normative specifications, 12 is the milestone
sequence with verification gates, 13–16 define success, scope limits, and standards.

---

## 0. Product summary

SoundPalette is a cross-platform desktop tool that treats a game's sound effects the way art
direction treats color: as a palette. It scans a folder of audio files, extracts perceptual
audio features (brightness, attack, decay tail, noisiness, warmth, loudness, grit), maps them
through a fixed, versioned mapping to visual attributes (hue, lightness, saturation, size,
spikiness, edge jitter, tail length), and renders every sound as a glyph in a grid. A cohesive
sound identity shows up as a cohesive palette; off-brand sounds are visible at a glance and can
be flagged automatically.

The deliverable of this plan is version 1, consisting of three artifacts built from one C++
codebase:

1. `libsoundpalette` — a headless static library: decode → normalize → features → mapping → manifest.
2. `soundpalette` CLI — `scan`, `lint`, `export-svg`, `watch`, `print-mapping`. Suitable for CI
   pipelines ("palette linting": fail the build when a new sound drifts off-palette).
3. `soundpalette-app` — a Dear ImGui (docking) + GLFW + OpenGL 3 desktop shell: glyph grid,
   click-to-play audio, hover details, and a live mapping tuner.

Primary target for v1 is Linux. The code must remain portable (no Linux-only APIs outside
clearly marked spots), but Windows/macOS packaging is out of scope for v1 (see §14).

---

## 1. Operating rules for the executing agent

1. Execute milestones **in order** (M0 → M7, §12). Each milestone ends with a **gate**: a set of
   commands with expected results. Do not start the next milestone until the gate passes.
2. **Never weaken a gate to make it pass.** Numeric tolerances in §11 are part of the spec. If a
   spec appears contradictory or impossible, stop, document the problem in `NOTES.md`, and
   report — do not silently change tests or constants.
3. Make one git commit per milestone at minimum, message format: `M<k>: <summary>`. Commit only
   when the milestone's gate is green.
4. At M0, pin every dependency to an exact tag or commit hash and record it in
   `DEPENDENCIES.md` (library, pin, license, purpose). §4 lists known-good versions; verify and
   pin the current stable at setup time.
5. Determinism is a feature. Given the same input folder, `scan` must produce byte-identical
   manifests across runs on the same machine (§8 rules: sorted entries, rounded floats, no
   timestamps, seeded randomness).
6. Layering is enforced: `core/` must not include or link GLFW, OpenGL, ImGui, or NFD. The CLI
   links core only. The app links core + UI libraries.
7. Add no third-party dependencies beyond §4 without recording a justification in `NOTES.md`.
8. At M0, also create `CLAUDE.md` at the repo root with the content given in §12/M0 so future
   sessions rediscover build and gate commands instantly.
9. All commands in this plan assume the repo root as working directory on Ubuntu 22.04+ with
   the packages from §2 installed. If a required package is missing and cannot be installed in
   the current environment, stop and report rather than working around it.

---

## 2. Prerequisites (Linux developer environment)

Target: Ubuntu 22.04 or 24.04 (Debian equivalents work; Fedora/Arch users translate package
names). One-shot install:

```bash
sudo apt-get update && sudo apt-get install -y \
  build-essential cmake ninja-build git pkg-config \
  xorg-dev libwayland-dev libxkbcommon-dev wayland-protocols \
  libgl1-mesa-dev \
  libasound2-dev libpulse-dev \
  libgtk-3-dev libdbus-1-dev \
  ffmpeg libxml2-utils xvfb
```

Purpose of each group:

| Packages | Needed for |
|---|---|
| build-essential, cmake (≥ 3.22), ninja-build, git, pkg-config | Toolchain. C++20 compiler required: GCC ≥ 11 or Clang ≥ 14. |
| xorg-dev, libwayland-dev, libxkbcommon-dev, wayland-protocols | GLFW window/input backends (X11 + Wayland). |
| libgl1-mesa-dev | OpenGL 3 headers/loader for the ImGui backend. |
| libasound2-dev, libpulse-dev | miniaudio playback backends (ALSA required, Pulse optional). |
| libgtk-3-dev, libdbus-1-dev | nativefiledialog-extended folder picker (GTK + portal). |
| ffmpeg | Test fixtures only: transcoding WAV fixtures to FLAC/OGG/MP3 (§11). Never a runtime dependency. |
| libxml2-utils | `xmllint` for the SVG well-formedness gate. |
| xvfb | Headless GUI smoke test (`xvfb-run`) in gates and CI. |

Verify before starting:

```bash
gcc --version         # ≥ 11 (or clang ≥ 14)
cmake --version       # ≥ 3.22
ninja --version
ffmpeg -version | head -n1
xmllint --version | head -n1
```

Executing agent: this plan is designed for Claude Code operating in the repo root. For current
Claude Code setup and usage details, consult the official docs rather than assumptions:
https://docs.claude.com/en/docs/claude-code/overview

---
## 3. Repository layout

```
soundpalette/
├── CMakeLists.txt              # top level: options, FetchContent, subdirs
├── cmake/                      # helper modules (warnings, sanitizer toggles)
├── extern/
│   ├── miniaudio/miniaudio.h   # vendored single header
│   ├── stb/stb_vorbis.c        # vendored (OGG decode via miniaudio integration)
│   └── stb/stb_image_write.h   # vendored (PNG for --smoke screenshot)
├── core/
│   ├── include/soundpalette/   # public headers (audio.h, features.h, mapping.h,
│   │                           #   manifest.h, glyph.h, lint.h, version.h)
│   └── src/                    # implementations, no UI/GL includes allowed
├── cli/
│   └── main.cpp                # subcommand dispatch: scan|lint|export-svg|watch|print-mapping
├── app/
│   ├── main.cpp                # GLFW/GL/ImGui bootstrap, --smoke mode
│   └── gui/                    # panels: grid, inspector, tuner
├── tools/
│   └── genfixtures/main.cpp    # deterministic test-audio generator (§11)
├── tests/
│   ├── unit/                   # doctest suites: test_decode.cpp, test_features.cpp,
│   │                           #   test_mapping.cpp, test_manifest.cpp, test_lint.cpp
│   ├── integration/            # bash scripts: golden_scan.sh, determinism.sh,
│   │                           #   lint_outlier.sh, svg_valid.sh, watch_update.sh, perf.sh
│   └── golden/                 # committed golden manifest + golden SVG
├── assets/mapping_v1.json      # mapping constants, exported by print-mapping (docs/tuner IO)
├── .github/workflows/ci.yml    # M7
├── .clang-format               # LLVM base style, 100 cols
├── CLAUDE.md                   # created at M0 (§12)
├── DEPENDENCIES.md             # pinned versions, created at M0
├── NOTES.md                    # deviations/justifications log
├── PLAN.md                     # this file
└── README.md                   # M7
```

---

## 4. Tech stack and dependencies

All permissively licensed; static linking into a single binary per platform is the goal.
Fetch via CMake `FetchContent` unless marked *vendored*. Known-good versions below; pin the
current stable tag/commit at M0 and record in `DEPENDENCIES.md`.

| Library | Known-good pin | Role | License |
|---|---|---|---|
| miniaudio (*vendored*) | 0.11.x | Decode WAV/FLAC/MP3, playback engine | MIT-0 / public domain |
| stb_vorbis (*vendored*) | latest stb | OGG Vorbis decode, integrated into miniaudio | MIT / public domain |
| stb_image_write (*vendored*) | latest stb | PNG write for `--smoke` | MIT / public domain |
| libebur128 | v1.2.6 | EBU R128 integrated loudness (LUFS) + true peak | MIT |
| KissFFT | 131.1.0 | Real FFT for STFT features | BSD-3 |
| nlohmann/json | v3.11.x | Manifest serialization, mapping IO | MIT |
| Dear ImGui, **docking branch** | ≥ 1.90.5 (needs `AddConcavePolyFilled`) | App UI | MIT |
| GLFW | 3.4 | Window/input (X11 + Wayland) | zlib |
| nativefiledialog-extended | v1.2.x | Native folder picker | zlib |
| doctest | v2.4.x | Unit test framework | MIT |

Integration notes:

- `MINIAUDIO_IMPLEMENTATION` must be defined in exactly one translation unit
  (`core/src/audio.cpp`). To enable OGG, include `stb_vorbis.c` alongside miniaudio exactly as
  described in the comment block at the top of `miniaudio.h` (the header documents the
  `STB_VORBIS_HEADER_ONLY` pattern) — read that section before wiring it.
- ImGui backends used: `imgui_impl_glfw` + `imgui_impl_opengl3`.
- Build KissFFT with `KISSFFT_DATATYPE=float`, tools/tests off.

---

## 5. Architecture

Data flow (core):

```
path ──decode──▶ AudioBuffer (mono float 48 kHz, ≤ 30 s analyzed)
      ──ebur128──▶ Loudness {lufs_i, true_peak_db, silent}
      ──normalize to −23 LUFS──▶ analysis copy
      ──STFT + envelope──▶ Features
      ──map_v1(Features, Loudness, seed)──▶ Visual
      ──aggregate──▶ Manifest ──▶ canonical JSON / SVG sheet / lint report
```

Public core API (signatures are normative; adjust internals freely):

```cpp
namespace sp {

struct AudioBuffer { std::vector<float> samples48k_mono; int src_rate = 0;
                     int src_channels = 0; double duration_s = 0; bool truncated = false; };

struct Loudness    { double lufs_i; double true_peak_db; bool silent; };

struct Features    { double centroid_hz, rolloff85_hz, flatness, zcr, attack_s, tail_s,
                     roughness, warmth; std::array<double, 6> bands; bool tail_clipped; };

struct Visual      { double hue_deg, sat, light, size_px, spike01, jitter01, tail01;
                     int spikes; std::uint64_t seed; };

std::optional<AudioBuffer> decode_file(const std::filesystem::path&, std::string& err);
Loudness measure_loudness(const AudioBuffer&);                 // on original gain
Features extract_features(const AudioBuffer&, const Loudness&); // on −23 LUFS copy
Visual   map_v1(const Features&, const Loudness&, std::uint64_t seed);
std::uint64_t path_seed(std::string_view relative_path);        // FNV-1a 64

struct FileEntry { std::string path, sha256; double duration_s; int sample_rate, channels;
                   Loudness loudness; Features features; Visual visual; std::string error; };
struct Manifest  { int schema_version, mapping_version; std::string engine_version, root;
                   std::vector<FileEntry> files; /* + per-feature stats block */ };

struct ScanOptions { int threads = 0 /*auto*/; bool include_meta = true; };
Manifest scan_directory(const std::filesystem::path& root, const ScanOptions&);
std::string manifest_to_json(const Manifest&);                  // canonical form, §8

std::vector<std::array<float,2>> glyph_outline(const Visual&, int base_points = 24);
std::string glyph_svg(const Visual&, double cell_px);           // shared by CLI + docs
std::string sheet_svg(const Manifest&, int columns);

struct LintReport { /* per-file max_z, worst feature, offending features */ };
LintReport lint(const Manifest& baseline, const Manifest& candidate, double threshold);

} // namespace sp
```

Threading: files are independent; `scan_directory` uses a fixed thread pool
(`threads == 0` → `std::thread::hardware_concurrency()`), collects results, then sorts by path
before serialization so output order never depends on scheduling. File discovery: recursive,
extensions `.wav .flac .ogg .mp3` (case-insensitive), paths stored relative to the scan root
with forward slashes. Files that fail to decode still get a manifest entry with `error` set and
are excluded from stats and lint.

---
## 6. DSP specification (normative)

**Preprocessing.** Decode to float32, downmix to mono (channel mean), resample to 48 000 Hz
(miniaudio resampler, default quality). Analyze at most the first 30 s; set `truncated = true`
beyond that. Loudness is measured on the **original-gain** audio: libebur128 with
`EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK`, reporting integrated LUFS and true peak in dBTP.
`silent = true` if integrated loudness is −∞/NaN or absolute peak < 1e-4 (≈ −80 dBFS); silent
files skip feature extraction and get the fixed "silent" visual (§7). For all further analysis,
scale the buffer so integrated loudness is −23 LUFS.

**STFT.** Window N = 2048, hop = 512, Hann window, power spectrum `P[k] = |X[k]|²` with floor
`1e-12`. Bin frequency `f[k] = k · 48000 / 2048`. Frames with total power < 1e-9 are skipped in
spectral averages.

**Spectral features** (average of per-frame values over non-skipped frames):

- `centroid_hz` — Σ f[k]·P[k] / Σ P[k].
- `rolloff85_hz` — lowest f[k] such that cumulative power ≥ 85 % of frame total.
- `flatness` — geometric mean / arithmetic mean of P[k] over 20 Hz–20 kHz bins, in [0, 1].
- `bands[6]` — fraction of total power in: sub 20–80, low 80–250, lowmid 250–600,
  mid 600–2500, high 2500–8000, air 8000–20000 Hz. Fractions sum to ≈ 1.
- `warmth` — `bands[low] + bands[lowmid]`.
- `roughness` (v1 proxy) — mean over frames of positive spectral flux,
  `Σ_k max(0, √P_t[k] − √P_{t−1}[k])`, normalized by `Σ_k √P_t[k]`. Documented placeholder; a
  modulation-based measure is v2 (§14).

**Time-domain features.** `zcr` — zero crossings per sample over the whole buffer.
Envelope `env[n]` — absolute value through a one-pole smoother with 5 ms time constant.
Let `peak = max(env)` at index `n_peak`.

- `attack_s` — time from the first sample where `env ≥ 0.10·peak` to the first subsequent
  sample where `env ≥ 0.90·peak`; floor at 0.0005 s.
- `tail_s` — time from `n_peak` to the last sample where `env ≥ peak·10^(−60/20)` (a T60-style
  measure). If the envelope never falls 60 dB below peak before EOF, `tail_s` = time from
  `n_peak` to EOF and `tail_clipped = true`.

---

## 7. Mapping v1 (feature → visual, normative constants)

Absolute mapping: constants are fixed so palettes are comparable across projects and over time.
Every constant lives in one `MappingConfig` struct with the defaults below (also exported to
`assets/mapping_v1.json` by `print-mapping`); the GUI tuner (§10) edits a runtime copy.
`mapping_version = 1`. Helper: `lin01(x, lo, hi) = clamp((x − lo)/(hi − lo), 0, 1)`;
`log01(x, lo, hi) = lin01(log10(x), log10(lo), log10(hi))`.

| Normalized value | Formula (defaults) |
|---|---|
| `bright01` | `lin01(log2(centroid_hz), log2(200), log2(8000))` |
| `warm01`   | `lin01(warmth, 0.10, 0.70)` |
| `ton01`    | `1 − lin01(flatness, 0, 0.50)` (tonal → 1, noisy → 0) |
| `loud01`   | `lin01(lufs_i, −40, −10)` (original loudness, not the normalized copy) |
| `atk01`    | `1 − log01(attack_s, 0.002, 0.150)` (fast attack → 1) |
| `tail01`   | `log01(tail_s, 0.05, 3.0)` |
| `flat01`   | `lin01(flatness, 0, 0.60)` |
| `rough01`  | `clamp(roughness · 4.0, 0, 1)` (constant marked TUNABLE) |
| `jitter01` | `0.7·flat01 + 0.3·rough01` |

Visual attributes (HSL color space for v1; OKLCH is v2):

- `hue_deg  = 220 − 200·warm01` (cool blue-violet → warm orange-red)
- `sat      = 25 + 60·ton01` (percent)
- `light    = 28 + 50·bright01` (percent)
- `size_px  = 14 + 50·loud01` (glyph radius at 100 % zoom)
- `spike01  = atk01`; `spikes = (atk01 > 0.35) ? round(4 + 10·atk01) : 0`
- `jitter01`, `tail01` pass through.

**Silent files:** hue 0, sat 0, light 60, size 8, spikes 0, jitter 0, tail 0.

**Glyph geometry** (shared by GUI and SVG via `glyph_outline`): a closed radial polygon of
24 base points. Radius per point `i`:
`r_i = size_px · (1 + spike_term_i + jitter_term_i)` where
`spike_term_i = spike01·0.45·max(0, cos(spikes · θ_i))` (0 when `spikes == 0`, producing a soft
blob) and `jitter_term_i = jitter01·0.30·(rand_i − 0.5)` with `rand_i` from xorshift64* seeded
by `Visual.seed = path_seed(relative_path)` — deterministic per file, stable across runs and
machines. Decay tail: 5 circles to the right of the glyph, radius shrinking from
`0.16·size_px`, opacity fading from 0.5 to 0.07, horizontal spread `tail01 · 2.2·size_px`;
omit when `tail01 < 0.05`.

---
## 8. Manifest schema (JSON)

Canonical serialization rules: UTF-8, LF line endings, 2-space indent, keys in the order shown,
files sorted ascending by `path` (byte order), all floating-point values rounded to 4 decimal
places, **no timestamps or absolute paths anywhere**. `scan --no-meta` omits the
`engine_version` field so golden tests survive version bumps. Example with one entry:

```json
{
  "schema_version": 1,
  "mapping_version": 1,
  "engine_version": "0.1.0",
  "root": "sfx",
  "file_count": 1,
  "files": [
    {
      "path": "impact/impact_flesh.wav",
      "sha256": "9f2c…",
      "error": "",
      "duration_s": 0.4215,
      "sample_rate": 44100,
      "channels": 2,
      "truncated": false,
      "loudness": { "lufs_i": -18.31, "true_peak_db": -1.2043, "silent": false },
      "features": {
        "centroid_hz": 612.4471, "rolloff85_hz": 1421.9032, "flatness": 0.3121,
        "zcr": 0.0512, "attack_s": 0.0041, "tail_s": 0.6112, "tail_clipped": false,
        "roughness": 0.1182, "warmth": 0.5533,
        "bands": [0.0812, 0.2911, 0.2622, 0.2410, 0.0981, 0.0264]
      },
      "visual": {
        "hue_deg": 68.9, "sat": 62.4, "light": 50.1, "size_px": 50.2,
        "spike01": 0.91, "spikes": 13, "jitter01": 0.41, "tail01": 0.61,
        "seed": 1469598103934665603
      }
    }
  ],
  "stats": {
    "bright01": { "mean": 0.44, "std": 0.09, "min": 0.21, "max": 0.61 },
    "warm01":  { "mean": 0.0,  "std": 0.0,  "min": 0.0,  "max": 0.0 }
  }
}
```

`stats` holds mean/std/min/max for the seven lint dimensions (§9) over all non-error,
non-silent files. Ship `docs/manifest.schema.json` (JSON Schema draft-07) describing the
above; a unit test validates the golden manifest against it structurally (required keys,
types, ranges).

---

## 9. CLI specification

Single binary `soundpalette`, subcommand style. Global flags: `--threads N`, `--quiet`.
Exit codes: `0` success, `1` lint outliers found, `2` usage/IO/decode-root errors.

```
soundpalette scan <dir> [--out palette.json] [--no-meta]
soundpalette lint <dir> --baseline palette.json [--threshold 2.5] [--top 10]
soundpalette export-svg <palette.json> --out sheet.svg [--columns 8]
soundpalette watch <dir> [--out palette.json] [--interval-ms 500]
soundpalette print-mapping [--out mapping.json]
```

- **scan** — writes the canonical manifest (stdout if `--out` omitted). Prints
  `scanned <n> files in <t> s` to stderr unless `--quiet`.
- **lint** — scans `<dir>`, compares each file against the **baseline's** `stats` block in the
  seven-dimensional mapping space `[bright01, warm01, ton01, atk01, tail01, loud01, jitter01]`.
  z-score per dimension with `std` floored at 0.02. A file is an outlier when any `|z| ≥
  threshold`. Output: `PASS <n> files within palette` or, per outlier sorted by max |z|
  descending (capped at `--top`):
  `OUTLIER <path> worst=<dim> z=<+d.dd> dims=<comma-list over threshold>`. Exit 1 if any.
- **export-svg** — grid sheet; cell 120 px, glyph centered, 11 px filename label beneath,
  mapping/schema versions in an XML comment. Deterministic bytes for a given manifest.
- **watch** — portable polling watcher (mtime + file set diff each interval); re-scans changed
  files only, rewrites the manifest atomically (temp file + rename). inotify is v2.
- **print-mapping** — dumps the active `MappingConfig` as JSON (matches `assets/mapping_v1.json`).

---

## 10. GUI specification (soundpalette-app)

Stack: GLFW + OpenGL 3 + Dear ImGui docking branch. Single window, default 1280×800, dark
theme. Layout: menu bar (Open folder… via NFD, Rescan, Export SVG, Quit) · left sidebar
(sort by hue / brightness / size / attack / tail / name; text filter) · central glyph grid ·
right inspector panel · bottom status bar (file count, scan time, mapping version).

- **Glyph grid** — one cell per manifest entry, `glyph_outline` rendered with ImDrawList
  `AddConcavePolyFilled` (requires ImGui ≥ 1.90.5) plus tail circles; filename beneath. Only
  visible rows are drawn (use `ImGuiListClipper`). Hover: tooltip with path, duration, LUFS,
  and the seven normalized dims as mini bars. Click: play the file through one `ma_engine`
  instance (original file path, not the analysis buffer). Error entries render as a gray ✕.
- **Inspector** — details of the selected file: all raw features, visual values, decode info.
- **Mapping tuner** — the payoff feature. Sliders for every `MappingConfig` constant (ranges
  ±50 % around defaults), grouped as in §7. Edits apply to a runtime copy and the entire grid
  re-derives `Visual` from cached `Features` live (no re-analysis, so it must stay 60 fps for
  2 000 files). Buttons: Reset to v1 · Export mapping.json (via NFD save dialog).
- **Rescan** — re-runs `scan_directory` on a worker thread; UI stays responsive; status bar
  shows progress `analyzed i/n`.
- **`--smoke <out.png> [--dir <folder>]`** — headless-friendly self test: if `--dir` given,
  scan it; create the window, render 30 frames, `glReadPixels` the framebuffer, write PNG via
  stb_image_write, exit 0. Any GL/init failure exits non-zero. Used with `xvfb-run` in gates.

---
## 11. Test fixtures and verification suite

**Fixture generator** — `tools/genfixtures <outdir> [--perf200]` writes deterministic WAV files
(48 kHz mono float32 unless noted). All randomness from xorshift64* seeded with
`0xDEADBEEFCAFEF00D` (+ file index where noted) so fixtures are byte-identical across runs.

| File | Content |
|---|---|
| `sine440_1s.wav` | 440 Hz sine, amplitude 0.5, 1.000 s |
| `sine997_cal.wav` | 997 Hz sine, peak amplitude 0.1001 (≈ −23 dBFS RMS), 2 s — LUFS calibration |
| `noise_white_1s.wav` | white noise, amplitude 0.25, 1 s |
| `click.wav` | 10 ms silence, 0.9 constant for 5 ms, exponential decay τ = 30 ms, total 0.5 s |
| `decay_t60.wav` | 1 kHz sine, instant onset, exponential decay τ = 0.0724 s (T60 = 0.5 s), 2 s |
| `darkset/dark_00..19.wav` | noise (seed + i) → one-pole low-pass fc 300 Hz applied twice → exp decay τ = 0.15 + 0.02·i → peak-normalize 0.4; 1.5 s |
| `bright_outlier.wav` | white noise → one-pole high-pass fc 3 kHz applied twice → exp decay τ = 0.05 s, peak 0.5, 0.4 s |
| `--perf200` | additionally writes `perf/p_000..199.wav`, 1 s noise each |

`tests/integration/make_lossy.sh` transcodes `sine440_1s.wav` to `.flac`, `.ogg`, `.mp3` with
ffmpeg (fixed bitrate flags) for decoder coverage.

**Unit tests** (doctest, run via ctest). Tolerances are normative:

| Test | Assertion |
|---|---|
| decode/duration | `sine440_1s.wav` → duration 1.000 ± 0.002 s, 48 000 Hz mono buffer |
| decode/lossy | flac/ogg/mp3 of the sine decode; RMS within 1 dB of the WAV's RMS |
| loudness/calibration | `sine997_cal.wav` → lufs_i = −23.0 ± 0.5 |
| features/centroid_sine | sine440 → centroid_hz ∈ [410, 470] |
| features/centroid_noise | white noise → centroid_hz ∈ [10 500, 13 500] (flat spectrum ≈ 12 kHz) |
| features/flatness | sine440 flatness < 0.05; white noise flatness > 0.45 |
| features/zcr | sine440 → zcr = 2·440/48000 ± 10 % |
| features/attack | click.wav → attack_s < 0.010; decay_t60 → attack_s < 0.012 |
| features/tail | decay_t60 → tail_s ∈ [0.40, 0.60] |
| mapping/monotonic | bright01, atk01, tail01 monotone in their inputs; all outputs within declared ranges; silent input → silent visual |
| mapping/determinism | `path_seed` and `glyph_outline` identical across two calls |
| manifest/schema | golden manifest satisfies `docs/manifest.schema.json` structural checks |

**Integration tests** (bash, exit 0 = pass):

| Script | Check |
|---|---|
| `golden_scan.sh` | `scan fixtures --no-meta` output equals committed `tests/golden/palette.json` byte-for-byte |
| `determinism.sh` | two consecutive scans produce identical bytes (`cmp`) |
| `lint_outlier.sh` | baseline = scan of `darkset/`; lint of darkset + `bright_outlier.wav` exits 1 and names `bright_outlier.wav`; lint of darkset alone exits 0 |
| `svg_valid.sh` | `export-svg` output passes `xmllint --noout`; two exports have equal sha256 |
| `watch_update.sh` | start `watch`; copy a new WAV in; manifest contains its path within 3 s; clean shutdown |
| `perf.sh` | `scan perf/ --threads 4` completes in ≤ 10 s wall on the 200-file set |
| `smoke` (M6) | `xvfb-run -a soundpalette-app --smoke out.png --dir fixtures` exits 0; PNG exists and is > 20 kB |

---

## 12. Milestones and gates

### M0 — Scaffold and toolchain
Create the §3 layout, top-level CMake (C++20, `-Wall -Wextra`, warnings-as-errors behind option
`SP_WERROR` default ON, Ninja generator), FetchContent for §4 pins, vendored headers, doctest
wired into ctest with one placeholder test, `.clang-format`, empty `NOTES.md`, `DEPENDENCIES.md`
with pins, git init + first commit. Create `CLAUDE.md` containing: one-paragraph project
summary, the build/test commands below, "read PLAN.md for all specs", and the rule "never
weaken gates".

**Gate:**
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: clean configure/build (no warnings), 1/1 tests pass.

### M1 — Decode, loudness, fixtures
Implement `genfixtures`, `decode_file` (miniaudio + stb_vorbis integration, downmix, resample,
30 s cap), `measure_loudness` (libebur128), `make_lossy.sh`.

**Gate:** `ctest -R "decode|loudness"` green (tests: decode/duration, decode/lossy,
loudness/calibration).

### M2 — Feature extraction
STFT (KissFFT real transform), spectral features, envelope, attack, tail, per §6.

**Gate:** `ctest -R features` green (all six feature tests of §11).

### M3 — Mapping, manifest, `scan`
`map_v1` + `MappingConfig` + `print-mapping`, `path_seed`, sha256 (vendor a small
public-domain implementation or use a header-only one; record in DEPENDENCIES.md),
`scan_directory` with thread pool, canonical serializer, CLI skeleton with `scan`.
Generate and commit `tests/golden/palette.json` from the fixtures **after** verifying values
by hand against §6/§7 spot-checks (document the spot-check in NOTES.md).

**Gate:** `ctest -R "mapping|manifest"` green; `golden_scan.sh`, `determinism.sh`, `perf.sh`
pass.

### M4 — `lint` and `export-svg`
Stats block, z-score lint, `glyph_outline`/`glyph_svg`/`sheet_svg`, both subcommands.

**Gate:** `lint_outlier.sh`, `svg_valid.sh` pass; `ctest -R lint` green.

### M5 — `watch`
Polling watcher with atomic manifest rewrite.

**Gate:** `watch_update.sh` passes.

### M6 — GUI shell
Everything in §10. Keep `app/` code thin; all analysis and geometry comes from core.

**Gate:**
```bash
cmake --build build --target soundpalette-app
xvfb-run -a ./build/app/soundpalette-app --smoke /tmp/smoke.png --dir <fixtures>
test -s /tmp/smoke.png && stat -c %s /tmp/smoke.png   # > 20480 bytes
```
Plus a manual checklist recorded in NOTES.md: grid renders, hover tooltip, click plays audio
(when a sound device exists; skip gracefully headless), tuner sliders re-color the grid live,
export-SVG menu item works.

### M7 — CI, docs, release hygiene
`.github/workflows/ci.yml`: ubuntu-latest, install §2 packages, configure/build, ctest, all
integration scripts, xvfb smoke, upload `sheet.svg` + `smoke.png` as artifacts. Write
`README.md` (what it is, quickstart, CLI examples, palette-lint-in-CI recipe, screenshot),
`LICENSES.md` (table from DEPENDENCIES.md). Tag `v0.1.0`.

**Gate:** full local pipeline green end-to-end:
```bash
ctest --test-dir build --output-on-failure && for s in tests/integration/*.sh; do bash "$s" || exit 1; done
```
and the workflow file passes `actionlint` if available (else visual review, noted in NOTES.md).

---
## 13. Success metrics (definition of "the software works")

1. **Functional** — every gate M0–M7 green; `ctest` fully green; all integration scripts exit 0.
2. **Deterministic** — `determinism.sh` and the double-export SVG hash check pass; golden
   manifest stable across repeated runs on the same machine.
3. **Correct by construction** — synthetic-signal assertions (§11) hold within stated
   tolerances; these anchor the DSP to ground truth rather than to itself.
4. **Fast enough** — 200 one-second files analyzed in ≤ 10 s at 4 threads (perf gate); GUI
   tuner stays interactive (~60 fps) at 2 000 glyphs.
5. **Useful as a product** — the lint demo works end to end: a dark-cohesive baseline flags a
   bright outlier with a readable message and exit code 1, suitable for CI.
6. **Clean** — builds warning-free with `-Wall -Wextra -Werror`; `clang-format --dry-run
   --Werror` clean; core has zero UI/GL includes (verify with `grep -rE "imgui|GLFW|GL/" core/`
   returning nothing).
7. **Honest** — NOTES.md contains any deviations, spot-check evidence for the golden manifest,
   and the M6 manual checklist results.

## 14. Non-goals for v1 (roadmap v2)

Explicitly out of scope now: neural embeddings (CLAP via ONNX Runtime) and a UMAP similarity
map; perceptual OKLCH color space; a true modulation-based roughness measure; inotify-based
watching; Windows/macOS packaging, code signing, notarization, AppImage; plugin formats;
localization. The architecture (core/CLI/app split, versioned mapping) is designed so each of
these lands without breaking v1 manifests.

## 15. Coding standards

C++20; standard library first. Exceptions allowed internally, never across the CLI boundary
(catch in `main`, map to exit code 2 with a message on stderr). RAII everywhere; no raw
`new/delete`. `.clang-format`: LLVM base, 100-column limit, 4-space indent. Naming:
`snake_case` functions/variables, `PascalCase` types, `SCREAMING_SNAKE` constants. Every §6/§7
formula implemented with a comment citing its PLAN.md section. No global mutable state except
the app's runtime `MappingConfig`.

## 16. Definition of done

- [ ] All milestone gates green, committed per §1 rule 3, tagged `v0.1.0`.
- [ ] `soundpalette scan/lint/export-svg/watch/print-mapping` behave exactly as §9.
- [ ] GUI meets §10 including `--smoke`; manual checklist recorded.
- [ ] README quickstart reproduced verbatim on a clean Ubuntu container succeeds.
- [ ] DEPENDENCIES.md, LICENSES.md, NOTES.md complete and truthful.
- [ ] A human can drop a folder of game SFX on it and see the palette. That was the point.
