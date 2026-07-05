#include <doctest/doctest.h>

#include <cmath>

#include "soundpalette/capability.h"
#include "soundpalette/manifest.h"
#include "soundpalette/mapping.h"
#include "soundpalette/profile.h"

namespace {

// Entries with controlled dims via synthetic features (no audio decode needed).
sp::FileEntry make_entry(const std::string &path, double centroid_hz, double warmth) {
    sp::FileEntry e;
    e.path = path;
    e.loudness.lufs_i = -20.0;
    e.loudness.silent = false;
    e.features.centroid_hz = centroid_hz;
    e.features.flatness = 0.2;
    e.features.attack_s = 0.02;
    e.features.tail_s = 0.5;
    e.features.roughness = 0.1;
    e.features.warmth = warmth;
    return e;
}

} // namespace

TEST_CASE("profile/from_manifest") {
    sp::Manifest m = sp::scan_directory("tests/golden/fixtures/darkset", sp::ScanOptions{});
    REQUIRE(m.files.size() == 20);

    sp::Profile p = sp::profile_from_manifest(m);
    CHECK(p.created_from_type == "manifest");
    CHECK(p.created_from_file_count == 20);
    CHECK(p.categories.empty()); // single anonymous top-level profile
    for (std::size_t d = 0; d < 8; ++d) {
        CHECK(p.stats[d].mean == doctest::Approx(m.stats[d].mean).epsilon(1e-12));
        CHECK(p.stats[d].std == doctest::Approx(m.stats[d].std).epsilon(1e-12));
        CHECK(p.cov[d][d] == doctest::Approx(m.stats[d].std * m.stats[d].std).epsilon(1e-9));
    }
}

TEST_CASE("profile/categories") {
    std::vector<sp::FileEntry> entries;
    for (int i = 0; i < 6; ++i) {
        entries.push_back(make_entry("ui/ui_0" + std::to_string(i) + ".wav", 6000.0, 0.05));
        entries.push_back(make_entry("combat/hit_0" + std::to_string(i) + ".wav", 300.0, 0.8));
    }
    entries.push_back(make_entry("stray.wav", 1000.0, 0.4));

    sp::Profile p = sp::profile_from_entries(
        entries, "test", "", 2.5, {{"ui", {"ui/**", "**/ui_*"}}, {"combat", {"combat/**"}}});

    CHECK(p.created_from_file_count == 13);
    REQUIRE(p.categories.size() == 2);
    CHECK(p.categories[0].file_count == 6);
    CHECK(p.categories[1].file_count == 6);

    // First match wins; unmatched -> top-level (index -1).
    CHECK(sp::resolve_category(p, "ui/ui_03.wav") == 0);
    CHECK(sp::resolve_category(p, "combat/hit_00.wav") == 1);
    CHECK(sp::resolve_category(p, "stray.wav") == -1);
    CHECK(sp::resolve_category(p, "x/y/ui_9.wav") == 0); // **/ui_* pattern
}

TEST_CASE("profile/cov") {
    std::vector<sp::FileEntry> entries;
    for (int i = 0; i < 10; ++i) {
        entries.push_back(
            make_entry("f" + std::to_string(i) + ".wav", 400.0 + 200.0 * i, 0.1 + 0.05 * i));
    }
    sp::Profile p = sp::profile_from_entries(entries, "cov", "", 2.5, {});

    for (std::size_t a = 0; a < 7; ++a) {
        CHECK(p.cov[a][a] == doctest::Approx(p.stats[a].std * p.stats[a].std).epsilon(1e-9));
        for (std::size_t b = 0; b < 7; ++b) {
            CHECK(p.cov[a][b] == doctest::Approx(p.cov[b][a]).epsilon(1e-12)); // symmetric
        }
    }

    // Deterministic bytes on double create, and JSON round-trip preserves the numbers.
    std::string j1 = sp::profile_to_json(p);
    std::string j2 = sp::profile_to_json(sp::profile_from_entries(entries, "cov", "", 2.5, {}));
    CHECK(j1 == j2);

    std::string err;
    auto back = sp::profile_from_json(j1, err);
    REQUIRE_MESSAGE(back.has_value(), err);
    CHECK(back->stats[0].mean == doctest::Approx(p.stats[0].mean).epsilon(1e-6));
}

TEST_CASE("seam/fail_open") {
    CHECK(sp::capability("harmonize.apply"));
    CHECK(sp::capability("mcp.write"));
    CHECK(sp::capability("profile.create"));
    CHECK(sp::capability("export.clean_sheet"));
    CHECK(sp::capability("some.unknown.future.feature")); // fail-open (§6.3)
}

TEST_CASE("profile/version_mismatch_refusal") {
    // Extension-3 §0: artifacts must match mapping_version AND ref_spl; refuse, never mix.
    sp::Profile p;
    p.mapping_version = 1;
    CHECK_FALSE(sp::profile_compat_error(p).empty());
    p.mapping_version = 2;
    p.ref_spl = 80.0;
    std::string err = sp::profile_compat_error(p);
    CHECK(err.find("ref_spl") != std::string::npos);
    p.ref_spl = 75.0;
    CHECK(sp::profile_compat_error(p).empty());
}
