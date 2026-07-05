#include <doctest/doctest.h>

#include <cmath>

#include "soundpalette/deviation.h"
#include "soundpalette/mapping.h"

namespace {

sp::FileEntry entry_with_dims_bright(double bright01_target) {
    // v2: sharpness such that bright01 = lin01(acum, 0.6, 3.5) equals the target.
    sp::FileEntry e;
    e.path = "t.wav";
    e.loudness.lufs_i = -25.0;
    e.loudness.silent = false;
    e.features.centroid_hz = 1200.0;
    e.features.flatness = 0.2;
    e.features.attack_s = 0.02;
    e.features.tail_s = 0.5;
    e.features.roughness = 0.1;
    e.features.warmth = 0.4;
    e.psycho.ref_spl = 75.0;
    e.psycho.sharpness_acum = 0.6 + bright01_target * (3.5 - 0.6);
    e.psycho.sones_n5 = 9.0; // loud01 = 0.5
    e.psycho.roughness_asper = 0.1;
    e.psycho.fluctuation_vacil = 0.2;
    return e;
}

// Profile centered exactly on the entry's own dims, std = 0.1 everywhere.
sp::Profile centered_profile(const sp::FileEntry &e, double threshold) {
    sp::Profile p;
    p.threshold = threshold;
    std::array<double, 8> dims = sp::mapping_dims(e.features, e.loudness, e.psycho);
    for (std::size_t d = 0; d < 8; ++d) {
        p.stats[d].mean = dims[d];
        p.stats[d].std = 0.1;
        p.cov[d][d] = 0.01;
    }
    return p;
}

} // namespace

TEST_CASE("deviation/z") {
    // Hand case (§8): mean 0.5, std 0.1, value 0.8 -> z = 3.0 exactly.
    sp::FileEntry e = entry_with_dims_bright(0.8);
    sp::Profile p = centered_profile(e, 2.5);
    p.stats[0].mean = 0.5;
    p.stats[0].std = 0.1;
    p.cov[0][0] = 0.01;

    sp::Deviation dev = sp::compute_deviation(e, p);
    CHECK(dev.z[0] == doctest::Approx(3.0).epsilon(1e-9));
    CHECK(dev.worst_dim == 0);

    // std floor at 0.02: std 0 behaves as 0.02.
    p.stats[0].std = 0.0;
    p.cov[0][0] = 0.0;
    dev = sp::compute_deviation(e, p);
    CHECK(dev.z[0] == doctest::Approx((0.8 - 0.5) / 0.02).epsilon(1e-9));
}

TEST_CASE("deviation/category_value") {
    // A ui tick: bright. ui category centered on it; combat centered far darker.
    sp::FileEntry tick = entry_with_dims_bright(0.9);
    tick.path = "ui/tick.wav";

    sp::Profile p = centered_profile(tick, 2.5);
    sp::CategoryProfile ui;
    ui.name = "ui";
    ui.match = {"ui/**"};
    ui.stats = p.stats; // centered on the tick -> conforming
    sp::CategoryProfile combat;
    combat.name = "combat";
    combat.match = {"combat/**"};
    combat.stats = p.stats;
    combat.stats[0].mean = 0.2; // combat family is dark; the tick is 0.9 -> z = 7
    p.categories = {ui, combat};

    sp::Deviation in_place = sp::compute_deviation(tick, p);
    CHECK(in_place.category == "ui");
    CHECK(in_place.band == sp::DevBand::none);

    tick.path = "combat/tick.wav"; // the same sound misfiled into combat
    sp::Deviation misfiled = sp::compute_deviation(tick, p);
    CHECK(misfiled.category == "combat");
    CHECK(misfiled.band == sp::DevBand::red); // per-category linting proves out
}

TEST_CASE("deviation/bands") {
    const double t = 2.5;
    sp::FileEntry e = entry_with_dims_bright(0.5);
    sp::Profile p = centered_profile(e, t);

    auto with_offset = [&](double z_target) {
        sp::Profile q = p;
        q.stats[0].mean -= z_target * 0.1; // std 0.1 -> max_z == z_target on dim 0
        return sp::compute_deviation(e, q).band;
    };

    CHECK(with_offset(0.79 * t) == sp::DevBand::none);
    CHECK(with_offset(0.80 * t) == sp::DevBand::amber);
    CHECK(with_offset(t) == sp::DevBand::red);
}
