#pragma once

#include <array>
#include <string>
#include <vector>

#include "soundpalette/manifest.h"
#include "soundpalette/mapping.h"
#include "soundpalette/profile.h"

namespace sp {

// Closed radial polygon outline for a glyph, in local coordinates centered at (0,0) (§7).
// Deterministic: re-seeded from visual.seed on every call (same Visual -> same outline).
// When spikes > 0 the star is a fixed five-point layout — two points up, three down, the
// horizontal axis left clear for the decay trail — and the sampling is snapped so every
// point angle is hit exactly (the lobes alias away otherwise).
std::vector<std::array<float, 2>> glyph_outline(const Visual &visual, int base_points = 48);

// Tonality rays (split-glyph revision): a fan of three polylines above the blob — one in
// each gap around/between the star's two upper points — in the same local coordinates as
// glyph_outline. Straight when tonal (ton01 = 1), sinusoidally wavy when noise-like
// (ton01 = 0). Deterministic; empty for silent visuals. Shared by the GUI renderer and the
// SVG sheet so both draw identical geometry.
std::vector<std::vector<std::array<float, 2>>> glyph_rays(const Visual &visual);

// Decay-tail half moons (trail revision): five fading crescents per side along the
// horizontal axis, concave side facing the blob. Each is a closed polygon in glyph-local
// coordinates with its fill opacity; empty when tail01 < 0.05. Shared by the GUI renderer
// and the SVG sheet so both draw identical geometry.
struct TailMoon {
    std::vector<std::array<float, 2>> pts;
    float opacity;
};
std::vector<TailMoon> glyph_tail(const Visual &visual);

// Standalone single-glyph SVG document (cell_px square), shared by the GUI and docs (§5/§7).
std::string glyph_svg(const Visual &visual, double cell_px);

// Grid sheet of every file in the manifest (§9 export-svg): cell 120 px, glyph centered, 11 px
// filename label beneath, mapping/schema versions in an XML comment. Deterministic bytes for a
// given manifest.
std::string sheet_svg(const Manifest &manifest, int columns, bool with_legend = true);

// Sheet with deviation halos (extension-2 §6.1): flagged glyphs get a ring behind them —
// red band solid #E24B4A, amber dashed #EF9F27 — carrying data-dev="red|amber" and a small
// "z x.x" label beneath. Entries need features+loudness populated for deviation computation.
std::string sheet_svg(const Manifest &manifest, int columns, const Profile &profile,
                      bool with_legend = true);

} // namespace sp
