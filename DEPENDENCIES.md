# Dependencies

Pinned at M0 (2026-07-04). All permissive licenses; static-linked into per-platform binaries.
See `cmake/Dependencies.cmake` for the FetchContent declarations (fetched ones) and
`extern/` for vendored single-file sources.

| Library | Pin | Mechanism | License | Purpose |
|---|---|---|---|---|
| miniaudio | tag `0.11.25` | vendored (`extern/miniaudio/miniaudio.h`) | MIT-0 / public domain | Decode WAV/FLAC/MP3, playback engine |
| stb_vorbis | commit `31c1ad37456438565541f4919958214b6e762fb4` (nothings/stb) | vendored (`extern/stb/stb_vorbis.c`) | MIT / public domain | OGG Vorbis decode, integrated into miniaudio |
| stb_image_write | commit `31c1ad37456438565541f4919958214b6e762fb4` (nothings/stb) | vendored (`extern/stb/stb_image_write.h`) | MIT / public domain | PNG write for `--smoke` |
| libebur128 | tag `v1.2.6` (jiixyj/libebur128) | FetchContent | MIT | EBU R128 integrated loudness (LUFS) + true peak |
| KissFFT | tag `131.1.0` (mborgerding/kissfft) | FetchContent | BSD-3 | Real FFT for STFT features |
| nlohmann/json | tag `v3.11.3` | FetchContent | MIT | Manifest serialization, mapping IO |
| Dear ImGui (docking) | tag `v1.92.8-docking` (satisfies plan's `>= 1.90.5`, has `AddConcavePolyFilled`) | FetchContent | MIT | App UI |
| GLFW | tag `3.4` | FetchContent | zlib | Window/input (X11 + Wayland) |
| nativefiledialog-extended | tag `v1.2.1` | FetchContent | zlib | Native folder picker |
| doctest | tag `v2.4.12` | FetchContent | MIT | Unit test framework |
| sha256 (Brad Conte, crypto-algorithms) | commit `cfbde48414baacf51fc7c74f275190881f037d32` | vendored (`extern/sha256/`) | Public domain | Manifest `sha256` per-file hash (PLAN.md §12/M3 explicitly sanctions vendoring a small public-domain implementation) |

PLAN.md §4 names libraries and known-good version numbers but not GitHub org/repo paths; those
were resolved by web search at M0 setup time and are recorded above (see NOTES.md for the
verification trail).

## npm (mcp/, pinned at M8 — extension §4; exact versions also locked in mcp/package-lock.json)

| Package | Pin | License | Purpose |
|---|---|---|---|
| @modelcontextprotocol/sdk | 1.29.0 | MIT | MCP server + test client, stdio transport |
| zod | 3.25.76 | MIT | Tool input schemas (peer requirement of the SDK) |
| @resvg/resvg-js | 2.6.2 | MPL-2.0 | Rasterize the SVG sheet to PNG for MCP image results |
| typescript | 5.9.3 (dev) | Apache-2.0 | Build mcp/src -> mcp/dist |
| @types/node | 20.19.9 (dev) | MIT | Type definitions for the Node 20 baseline |

## Development-time oracles (extension-3 §2; never runtime or user-facing dependencies)

| Package | Pin | License | Purpose |
|---|---|---|---|
| MoSQITo | 1.2.1 (pip) | Apache-2.0 | Psychoacoustic reference values (ISO 532-1 loudness, DIN 45692 sharpness, Daniel & Weber roughness) for `tests/golden/psycho_reference.json`; numbers only, no code ported |
| numpy | 2.5.1 (pip) | BSD-3-Clause | Oracle script array math |
| scipy | 1.18.0 (pip) | BSD-3-Clause | Oracle script WAV loading (`scipy.io.wavfile`) |
| matplotlib | 3.11.0 (pip) | PSF-based (matplotlib license) | Transitive import required by MoSQITo 1.2.1 at module load; unused by the oracle script itself |

Environment: `python3 -m venv .venv-psycho && . .venv-psycho/bin/activate && pip install "mosqito==1.2.1" scipy numpy matplotlib` (git-ignored). MoSQITo 1.2.1 does not export fluctuation strength; the §5 definitional gates stand alone for that metric (recorded in NOTES.md).
