#include <doctest/doctest.h>

#include <cmath>
#include <set>

#include "soundpalette/deviation.h"
#include "soundpalette/presets.h"
#include "soundpalette/profile.h"

TEST_CASE("presets/list_and_lookup") {
    const std::vector<sp::PresetInfo> &list = sp::builtin_preset_list();
    CHECK(list.size() == 4);
    std::set<std::string> slugs;
    for (const sp::PresetInfo &info : list) {
        CHECK(!info.name.empty());
        CHECK(!info.description.empty());
        CHECK(slugs.insert(info.slug).second); // slugs unique
        CHECK(sp::builtin_preset(info.slug).has_value());
    }
    CHECK(!sp::builtin_preset("no-such-genre").has_value());
}

TEST_CASE("presets/designed_stats_sane") {
    for (const sp::PresetInfo &info : sp::builtin_preset_list()) {
        sp::Profile p = *sp::builtin_preset(info.slug);
        CHECK(p.name == info.name);
        CHECK(p.created_from_type == "designed");
        CHECK(p.created_from_file_count == 0);
        CHECK(p.threshold == doctest::Approx(2.5));
        CHECK(p.categories.empty());
        for (std::size_t d = 0; d < 8; ++d) {
            // Authored priors: means inside the mapping range, generous non-zero stds,
            // diagonal covariance == std² (no correlation claimed), full min/max range.
            CHECK(p.stats[d].mean >= 0.0);
            CHECK(p.stats[d].mean <= 1.0);
            CHECK(p.stats[d].std >= 0.1);
            CHECK(p.stats[d].std <= 0.5);
            CHECK(p.cov[d][d] == doctest::Approx(p.stats[d].std * p.stats[d].std));
            CHECK(p.stats[d].min == 0.0);
            CHECK(p.stats[d].max == 1.0);
            for (std::size_t e = 0; e < 7; ++e) {
                if (e != d) {
                    CHECK(p.cov[d][e] == 0.0);
                }
            }
        }
    }
}

TEST_CASE("presets/json_roundtrip") {
    // Presets are ordinary Profiles: canonical JSON out, parse back, same stats.
    for (const sp::PresetInfo &info : sp::builtin_preset_list()) {
        sp::Profile p = *sp::builtin_preset(info.slug);
        std::string err;
        std::optional<sp::Profile> back = sp::profile_from_json(sp::profile_to_json(p), err);
        REQUIRE_MESSAGE(back.has_value(), err);
        CHECK(back->name == p.name);
        CHECK(back->created_from_type == "designed");
        CHECK(back->threshold == doctest::Approx(p.threshold));
        for (std::size_t d = 0; d < 8; ++d) {
            CHECK(back->stats[d].mean == doctest::Approx(p.stats[d].mean));
            CHECK(back->stats[d].std == doctest::Approx(p.stats[d].std));
        }
    }
}

TEST_CASE("presets/deviation_direction") {
    // A canonical chip blip (instant attack, no tail, tonal) must conform to Retro 8-bit and
    // stray from Horror — the guardrails have to disagree in the right direction.
    sp::FileEntry blip;
    blip.path = "blip.wav";
    blip.loudness.lufs_i = -23.0; // loud01 ~ .57
    blip.loudness.silent = false;
    blip.features.centroid_hz = 2000.0; // bright01 ~ .62
    blip.features.flatness = 0.05;      // tonal
    blip.features.attack_s = 0.003;     // near-instant
    blip.features.tail_s = 0.08;        // barely any tail
    blip.features.roughness = 0.05;
    blip.psycho.ref_spl = 75.0;
    blip.psycho.sones_n5 = 20.0;         // loud01 ~ .75 (consistent chip output level)
    blip.psycho.sharpness_acum = 1.9;    // bright01 ~ .45
    blip.psycho.roughness_asper = 0.15;  // jitter01 ~ .22 (pure tone, slight edge)
    blip.psycho.fluctuation_vacil = 0.5; // fluct01 ~ .49 (short blip, mild envelope)
    blip.features.warmth = 0.25;

    sp::Deviation retro = sp::compute_deviation(blip, *sp::builtin_preset("retro-8bit"));
    sp::Deviation horror = sp::compute_deviation(blip, *sp::builtin_preset("horror"));
    CHECK(retro.band == sp::DevBand::none);
    CHECK(horror.band != sp::DevBand::none);
    CHECK(horror.max_z > retro.max_z);
}
