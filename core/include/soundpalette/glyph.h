#pragma once

#include <array>
#include <string>
#include <vector>

#include "soundpalette/manifest.h"
#include "soundpalette/mapping.h"

namespace sp {

// Closed radial polygon outline for a glyph, in local coordinates centered at (0,0) (§7).
// Deterministic: re-seeded from visual.seed on every call (same Visual -> same outline).
std::vector<std::array<float, 2>> glyph_outline(const Visual &visual, int base_points = 24);

// Standalone single-glyph SVG document (cell_px square), shared by the GUI and docs (§5/§7).
std::string glyph_svg(const Visual &visual, double cell_px);

// Grid sheet of every file in the manifest (§9 export-svg): cell 120 px, glyph centered, 11 px
// filename label beneath, mapping/schema versions in an XML comment. Deterministic bytes for a
// given manifest.
std::string sheet_svg(const Manifest &manifest, int columns);

} // namespace sp
