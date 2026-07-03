#include <doctest/doctest.h>

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

bool in_unit_range(double x) {
    return x >= 0.0 && x <= 1.0;
}

} // namespace

TEST_CASE("mapping/monotonic") {
    sp::reset_active_mapping_config();

    SUBCASE("bright01 (via light) increases with centroid_hz") {
        sp::Features low = make_features(300.0, 0.02, 0.5);
        sp::Features mid = make_features(2000.0, 0.02, 0.5);
        sp::Features high = make_features(6000.0, 0.02, 0.5);
        sp::Loudness l = make_loudness(-20.0);

        sp::Visual v_low = sp::map_v1(low, l, 1);
        sp::Visual v_mid = sp::map_v1(mid, l, 1);
        sp::Visual v_high = sp::map_v1(high, l, 1);

        CHECK(v_low.light < v_mid.light);
        CHECK(v_mid.light < v_high.light);
    }

    SUBCASE("atk01 (via spike01) decreases as attack_s increases (faster attack -> higher atk01)") {
        sp::Features fast = make_features(1000.0, 0.003, 0.5);
        sp::Features mid = make_features(1000.0, 0.02, 0.5);
        sp::Features slow = make_features(1000.0, 0.10, 0.5);
        sp::Loudness l = make_loudness(-20.0);

        sp::Visual v_fast = sp::map_v1(fast, l, 1);
        sp::Visual v_mid = sp::map_v1(mid, l, 1);
        sp::Visual v_slow = sp::map_v1(slow, l, 1);

        CHECK(v_fast.spike01 > v_mid.spike01);
        CHECK(v_mid.spike01 > v_slow.spike01);
    }

    SUBCASE("tail01 increases with tail_s") {
        sp::Features short_tail = make_features(1000.0, 0.02, 0.08);
        sp::Features mid_tail = make_features(1000.0, 0.02, 0.5);
        sp::Features long_tail = make_features(1000.0, 0.02, 2.0);
        sp::Loudness l = make_loudness(-20.0);

        sp::Visual v_short = sp::map_v1(short_tail, l, 1);
        sp::Visual v_mid = sp::map_v1(mid_tail, l, 1);
        sp::Visual v_long = sp::map_v1(long_tail, l, 1);

        CHECK(v_short.tail01 < v_mid.tail01);
        CHECK(v_mid.tail01 < v_long.tail01);
    }

    SUBCASE("outputs stay within declared ranges across a range of inputs") {
        sp::Loudness l = make_loudness(-25.0);
        for (double centroid = 50.0; centroid <= 20000.0; centroid *= 2.0) {
            for (double attack = 0.0005; attack <= 0.5; attack *= 3.0) {
                sp::Features f = make_features(centroid, attack, 1.0);
                sp::Visual v = sp::map_v1(f, l, 1);
                CHECK(in_unit_range(v.spike01));
                CHECK(in_unit_range(v.jitter01));
                CHECK(in_unit_range(v.tail01));
                CHECK(v.sat >= 25.0);
                CHECK(v.sat <= 85.0);
                CHECK(v.light >= 28.0);
                CHECK(v.light <= 78.0);
                CHECK(v.size_px >= 14.0);
                CHECK(v.size_px <= 64.0);
            }
        }
    }

    SUBCASE("silent input -> fixed silent visual") {
        sp::Features f = make_features(1000.0, 0.02, 0.5);
        sp::Loudness silent = make_loudness(-70.0, true);
        sp::Visual v = sp::map_v1(f, silent, 42);
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
}
