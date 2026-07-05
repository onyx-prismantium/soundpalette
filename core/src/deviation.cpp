// The single deviation implementation shared by CLI, MCP, SVG export, and GUI (extension-2 §5).

#include <algorithm>
#include <cmath>

#include "soundpalette/deviation.h"
#include "soundpalette/mapping.h"

namespace sp {

Deviation compute_deviation(const FileEntry &entry, const Profile &profile) {
    Deviation dev;
    if (!entry.error.empty() || entry.loudness.silent) {
        return dev; // band none, excluded from stats (§5)
    }

    const int cat = resolve_category(profile, entry.path);
    const std::array<DimStats, 8> &stats =
        cat >= 0 ? profile.categories[static_cast<std::size_t>(cat)].stats : profile.stats;
    dev.category = cat >= 0 ? profile.categories[static_cast<std::size_t>(cat)].name : "";

    const std::array<double, 8> dims = mapping_dims(entry.features, entry.loudness, entry.psycho);
    for (std::size_t d = 0; d < 8; ++d) {
        dev.z[d] = (dims[d] - stats[d].mean) / std::max(stats[d].std, 0.02); // floor at use
        if (std::fabs(dev.z[d]) > dev.max_z) {
            dev.max_z = std::fabs(dev.z[d]);
            dev.worst_dim = static_cast<int>(d);
        }
    }

    const double t = profile.threshold;
    if (dev.max_z >= t) {
        dev.band = DevBand::red;
    } else if (dev.max_z >= 0.8 * t) {
        dev.band = DevBand::amber;
    }
    return dev;
}

const char *dev_band_name(DevBand band) {
    switch (band) {
    case DevBand::red:
        return "red";
    case DevBand::amber:
        return "amber";
    case DevBand::none:
        break;
    }
    return "none";
}

} // namespace sp
