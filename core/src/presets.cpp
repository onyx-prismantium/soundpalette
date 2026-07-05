// Built-in designed genre presets. Every number here is an authored prior, not a corpus
// measurement: means say where a genre's palette usually sits on each mapping dim, stds are
// deliberately generous so the preset guardrails rather than lints strictly, cov is diagonal
// (std² — no cross-dim correlation claimed). Calibration anchor (mapping v2, ref_spl 75): a
// broad organic reference pack (2.7k game SFX) measures bright .38±.19, warm .40±.36,
// ton .86±.22, atk .32±.30, tail .57±.21, loud .75±.20, jitter .69±.38, fluct .82±.30 —
// designed means shift from that anchor in each genre's characteristic dims.
// Dim order everywhere: [bright01, warm01, ton01, atk01, tail01, loud01, jitter01, fluct01].

#include "soundpalette/presets.h"

#include <array>

#include "soundpalette/mapping.h"

namespace sp {

namespace {

Profile make_designed(const std::string &name, const std::string &description,
                      const std::array<std::array<double, 2>, 8> &mean_std) {
    Profile p;
    p.mapping_version = active_mapping_config().mapping_version;
    p.ref_spl = active_mapping_config().ref_spl;
    p.name = name;
    p.description = description;
    p.created_from_type = "designed";
    p.created_from_root = "";
    p.created_from_file_count = 0;
    for (std::size_t d = 0; d < 8; ++d) {
        p.stats[d].mean = mean_std[d][0];
        p.stats[d].std = mean_std[d][1];
        p.stats[d].min = 0.0; // authored prior: the full mapping range is possible
        p.stats[d].max = 1.0;
        p.cov[d][d] = mean_std[d][1] * mean_std[d][1];
    }
    return p;
}

} // namespace

const std::vector<PresetInfo> &builtin_preset_list() {
    static const std::vector<PresetInfo> kList = {
        {"sci-fi", "Sci-Fi",
         "Bright, synthetic, tonal; fast attacks, controlled tails, tight loudness"},
        {"horror", "Horror", "Dark, noisy, slow swells; long tails, quiet-skewed dynamics"},
        {"fantasy", "Fantasy", "Warm, acoustic, organic; natural attacks and decays"},
        {"retro-8bit", "Retro 8-bit",
         "Chip-style: hard attacks, near-zero tails, pure tones plus noise-channel grit"},
    };
    return kList;
}

std::optional<Profile> builtin_preset(std::string_view slug) {
    // Order: bright01, warm01, ton01, atk01, tail01, loud01, jitter01, fluct01.
    if (slug == "sci-fi") {
        // Synth sources raise sharpness and stay tonal; sound design is mixed hot and tight;
        // attacks lean fast but sweeps/pads keep the std wide; controlled modulation.
        return make_designed("Sci-Fi", "Designed preset: bright synthetic palette",
                             {{{0.55, 0.18},
                               {0.30, 0.20},
                               {0.80, 0.20},
                               {0.55, 0.25},
                               {0.45, 0.22},
                               {0.78, 0.15},
                               {0.45, 0.28},
                               {0.60, 0.30}}});
    }
    if (slug == "horror") {
        // Low sharpness and rough textures; swells over transients (slow attacks), long
        // reverberant tails, quiet-skewed dynamics, strong slow modulation (breathing beds).
        return make_designed("Horror", "Designed preset: dark noisy palette",
                             {{{0.22, 0.15},
                               {0.40, 0.25},
                               {0.60, 0.25},
                               {0.25, 0.20},
                               {0.70, 0.20},
                               {0.60, 0.22},
                               {0.75, 0.30},
                               {0.85, 0.25}}});
    }
    if (slug == "fantasy") {
        // Hugs the organic anchor on purpose: acoustic/foley sources, natural decays,
        // moderate everything — the widest net of the four.
        return make_designed("Fantasy", "Designed preset: warm organic palette",
                             {{{0.40, 0.22},
                               {0.45, 0.30},
                               {0.85, 0.18},
                               {0.40, 0.28},
                               {0.55, 0.22},
                               {0.70, 0.20},
                               {0.60, 0.30},
                               {0.75, 0.28}}});
    }
    if (slug == "retro-8bit") {
        // Chip constraints: instant envelopes (atk mean .85, tight), effectively no decay
        // tail, consistent output level, pure tones with low roughness (the noise channel
        // widens the stds), moderate fluctuation from arps and vibrato.
        return make_designed("Retro 8-bit", "Designed preset: chip-style palette",
                             {{{0.45, 0.18},
                               {0.25, 0.18},
                               {0.85, 0.20},
                               {0.85, 0.15},
                               {0.15, 0.12},
                               {0.75, 0.15},
                               {0.35, 0.25},
                               {0.55, 0.30}}});
    }
    return std::nullopt;
}

} // namespace sp
