#include <doctest/doctest.h>

#include "soundpalette/glyph.h"
#include "soundpalette/mapping.h"

namespace {

sp::Features make_features(double centroid_hz, double attack_s, double tail_s) {
    sp::Features f;
    f.centroid_hz = centroid_hz;
    f.attack_s = attack_s;
    f.tail_s = tail_s;
    f.flatness = 0.2;
    f.warmth = 0.3;
    f.roughness = 0.05;
    return f;
}

sp::Loudness make_loudness(double lufs_i, bool silent = false) {
    sp::Loudness l;
    l.lufs_i = lufs_i;
    l.true_peak_db = -3.0;
    l.silent = silent;
    return l;
}

// v2 dims read the psychoacoustic block (extension-3 §6).
sp::PsychoFeatures make_psycho(double sones_n5, double acum, double asper = 0.05,
                               double vacil = 0.1) {
    sp::PsychoFeatures p;
    p.ref_spl = 75.0;
    p.sones_n5 = sones_n5;
    p.sones_mean = sones_n5 * 0.9;
    p.sharpness_acum = acum;
    p.roughness_asper = asper;
    p.fluctuation_vacil = vacil;
    return p;
}

bool in_unit_range(double x) {
    return x >= 0.0 && x <= 1.0;
}

} // namespace

TEST_CASE("mapping/monotonic") {
    sp::reset_active_mapping_config();

    SUBCASE("bright01 (via light) increases with sharpness (v2)") {
        sp::Features f = make_features(1000.0, 0.02, 0.5);
        sp::Loudness l = make_loudness(-20.0);

        sp::Visual v_low = sp::map_v2(f, l, make_psycho(4.0, 0.8), 1);
        sp::Visual v_mid = sp::map_v2(f, l, make_psycho(4.0, 1.8), 1);
        sp::Visual v_high = sp::map_v2(f, l, make_psycho(4.0, 3.0), 1);

        CHECK(v_low.light < v_mid.light);
        CHECK(v_mid.light < v_high.light);
    }

    SUBCASE("size area is honest to sones (v2): 4x sones -> ~2x size_px") {
        sp::Features f = make_features(1000.0, 0.02, 0.5);
        sp::Loudness l = make_loudness(-20.0);
        sp::Visual v1x = sp::map_v2(f, l, make_psycho(4.0, 1.0), 1);
        sp::Visual v4x = sp::map_v2(f, l, make_psycho(16.0, 1.0), 1);
        // size = 10 + 11*sqrt(sones): compare the sone-driven parts (area ratio == sones
        // ratio within rounding once the base offset is removed).
        const double r = (v4x.size_px - 10.0) / (v1x.size_px - 10.0);
        CHECK(r == doctest::Approx(2.0).epsilon(0.01));
    }

    SUBCASE("fluct01 maps fluctuation strength and reaches the glyph") {
        sp::Features f = make_features(1000.0, 0.02, 0.5);
        sp::Loudness l = make_loudness(-20.0);
        sp::Visual steady = sp::map_v2(f, l, make_psycho(4.0, 1.0, 0.05, 0.0), 1);
        sp::Visual wobbly = sp::map_v2(f, l, make_psycho(4.0, 1.0, 0.05, 0.9), 1);
        CHECK(steady.fluct01 < 0.05);
        CHECK(wobbly.fluct01 > 0.8);
    }

    SUBCASE("atk01 (via spike01) decreases as attack_s increases (faster attack -> higher atk01)") {
        sp::Features fast = make_features(1000.0, 0.003, 0.5);
        sp::Features mid = make_features(1000.0, 0.02, 0.5);
        sp::Features slow = make_features(1000.0, 0.10, 0.5);
        sp::Loudness l = make_loudness(-20.0);

        sp::Visual v_fast = sp::map_v2(fast, l, make_psycho(4.0, 1.0), 1);
        sp::Visual v_mid = sp::map_v2(mid, l, make_psycho(4.0, 1.0), 1);
        sp::Visual v_slow = sp::map_v2(slow, l, make_psycho(4.0, 1.0), 1);

        CHECK(v_fast.spike01 > v_mid.spike01);
        CHECK(v_mid.spike01 > v_slow.spike01);
    }

    SUBCASE("tail01 increases with tail_s") {
        sp::Features short_tail = make_features(1000.0, 0.02, 0.08);
        sp::Features mid_tail = make_features(1000.0, 0.02, 0.5);
        sp::Features long_tail = make_features(1000.0, 0.02, 2.0);
        sp::Loudness l = make_loudness(-20.0);

        sp::Visual v_short = sp::map_v2(short_tail, l, make_psycho(4.0, 1.0), 1);
        sp::Visual v_mid = sp::map_v2(mid_tail, l, make_psycho(4.0, 1.0), 1);
        sp::Visual v_long = sp::map_v2(long_tail, l, make_psycho(4.0, 1.0), 1);

        CHECK(v_short.tail01 < v_mid.tail01);
        CHECK(v_mid.tail01 < v_long.tail01);
    }

    SUBCASE("outputs stay within declared ranges across a range of inputs") {
        sp::Loudness l = make_loudness(-25.0);
        for (double sones = 0.0; sones <= 60.0; sones += 7.5) {
            for (double attack = 0.0005; attack <= 0.5; attack *= 3.0) {
                sp::Features f = make_features(2000.0, attack, 1.0);
                sp::Visual v = sp::map_v2(f, l, make_psycho(sones, 0.5 + sones / 15.0), 1);
                CHECK(in_unit_range(v.spike01));
                CHECK(in_unit_range(v.jitter01));
                CHECK(in_unit_range(v.tail01));
                CHECK(in_unit_range(v.fluct01));
                CHECK(v.sat >= 25.0);
                CHECK(v.sat <= 85.0);
                CHECK(v.light >= 28.0);
                CHECK(v.light <= 78.0);
                CHECK(v.size_px >= 12.0);
                CHECK(v.size_px <= 72.0);
            }
        }
    }

    SUBCASE("silent input -> fixed silent visual") {
        sp::Features f = make_features(1000.0, 0.02, 0.5);
        sp::Loudness silent = make_loudness(-70.0, true);
        sp::Visual v = sp::map_v2(f, silent, make_psycho(0.0, 0.0), 42);
        CHECK(v.hue_deg == 0.0);
        CHECK(v.sat == 0.0);
        CHECK(v.light == 60.0);
        CHECK(v.size_px == 8.0);
        CHECK(v.spikes == 0);
        CHECK(v.jitter01 == 0.0);
        CHECK(v.tail01 == 0.0);
        CHECK(v.seed == 42);
    }
}

TEST_CASE("mapping/determinism") {
    CHECK(sp::path_seed("impact/impact_flesh.wav") == sp::path_seed("impact/impact_flesh.wav"));
    CHECK(sp::path_seed("a.wav") != sp::path_seed("b.wav"));

    // glyph_outline identical across two calls for the same Visual (§11): the jitter RNG must
    // be re-seeded from Visual.seed per call, not carried across calls.
    sp::Visual v;
    v.hue_deg = 120.0;
    v.sat = 60.0;
    v.light = 50.0;
    v.size_px = 40.0;
    v.spike01 = 0.7;
    v.spikes = 9;
    v.jitter01 = 0.5;
    v.tail01 = 0.3;
    v.seed = sp::path_seed("impact/impact_flesh.wav");

    auto a = sp::glyph_outline(v);
    auto b = sp::glyph_outline(v);
    REQUIRE(a.size() == 24);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i][0] == b[i][0]);
        CHECK(a[i][1] == b[i][1]);
    }

    // A different seed must change the jittered outline.
    sp::Visual v2 = v;
    v2.seed = sp::path_seed("other/file.wav");
    auto c = sp::glyph_outline(v2);
    bool any_diff = false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i][0] != c[i][0] || a[i][1] != c[i][1]) {
            any_diff = true;
        }
    }
    CHECK(any_diff);
}
