#pragma once

#include <array>
#include <string>

#include "soundpalette/manifest.h"
#include "soundpalette/profile.h"

namespace sp {

// Extension-2 §5: bands always pair with a second visual cue at render time (never color
// alone). Silent/error files get band none and never contribute to stats.
enum class DevBand { none, amber, red };

struct Deviation {
    std::array<double, 7> z{};
    double max_z = 0.0;
    int worst_dim = 0;
    std::string category; // resolved category name, "" = top-level
    DevBand band = DevBand::none;
};

// The one deviation implementation every surface uses (CLI, MCP, SVG, GUI): z per dim against
// the file's resolved category stats (std floored at 0.02 at use time); red at
// max_z >= profile.threshold, amber at 0.8*threshold (§5).
Deviation compute_deviation(const FileEntry &entry, const Profile &profile);

const char *dev_band_name(DevBand band); // "none" | "amber" | "red"

} // namespace sp
