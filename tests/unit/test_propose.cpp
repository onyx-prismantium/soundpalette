#include <doctest/doctest.h>

#include <array>

#include "soundpalette/manifest.h"
#include "soundpalette/mapping.h"
#include "soundpalette/propose.h"
#include "soundpalette/recipe.h"

namespace {

// v1 fixture-set baseline stats, computed live (fast; ~30 files).
std::array<sp::DimStats, 8> fixture_baseline() {
    sp::Manifest m = sp::scan_directory("tests/golden/fixtures", sp::ScanOptions{});
    REQUIRE(m.files.size() > 20);
    return m.stats;
}

} // namespace

TEST_CASE("propose/deterministic") {
    std::string err;
    auto audio = sp::decode_file_native("fixtures_m9/fixable_outlier.wav", err);
    REQUIRE_MESSAGE(audio.has_value(), err);
    sp::Loudness loudness;
    sp::Features features;
    sp::PsychoFeatures psycho;
    sp::analyze_native(*audio, loudness, features, psycho);
    const auto stats = fixture_baseline();

    sp::Recipe a = sp::propose_recipe(*audio, features, loudness, psycho, stats, 2.5);
    sp::Recipe b = sp::propose_recipe(*audio, features, loudness, psycho, stats, 2.5);
    CHECK(sp::recipe_to_json(a) == sp::recipe_to_json(b)); // byte-identical, twice (§6.4: pure)
    CHECK(a.result_max_z_after < a.result_max_z_before);
}

TEST_CASE("propose/unresolved") {
    // Synthetic case (§7): only ton01 offends -> no tier-one inverse, so ops stay empty (no
    // loudness offense means no final gain either) and unresolved names ton01.
    sp::Features f;
    f.centroid_hz = 1000.0;
    f.flatness = 0.45; // very noisy per ton01...
    f.zcr = 0.05;
    f.attack_s = 0.02;
    f.tail_s = 0.5;
    f.roughness = 0.0; // ...but keep jitter01's rough half at zero
    f.warmth = 0.4;
    sp::Loudness l;
    l.lufs_i = -25.0;
    l.silent = false;
    sp::PsychoFeatures psy;
    psy.ref_spl = 75.0;
    psy.sones_n5 = 9.0;
    psy.sharpness_acum = 1.5;
    psy.roughness_asper = 0.1;
    psy.fluctuation_vacil = 0.2;

    // Baseline centered exactly on this file's dims, except ton01 far away.
    std::array<double, 8> dims = sp::mapping_dims(f, l, psy);
    std::array<sp::DimStats, 8> stats{};
    for (std::size_t d = 0; d < 8; ++d) {
        stats[d].mean = dims[d];
        stats[d].std = 0.1;
    }
    stats[2].mean = dims[2] + 0.9; // ton01 offense (z = -9 with std 0.1)

    sp::NativeAudio dummy;
    dummy.rate = 48000;
    dummy.channels.resize(1, std::vector<float>(1024, 0.1f));

    sp::Recipe r = sp::propose_recipe(dummy, f, l, psy, stats, 2.5);
    CHECK(r.ops.empty());
    REQUIRE(r.result_unresolved.size() == 1);
    CHECK(r.result_unresolved[0] == "ton01");
    CHECK_FALSE(r.result_converged);
}

TEST_CASE("recipe/order_validation") {
    sp::Op gain;
    gain.op = sp::OpType::kGainToLufs;
    sp::Op shelf;
    shelf.op = sp::OpType::kHighShelf;
    shelf.freq_hz = 4000.0;
    shelf.gain_db = -6.0;

    std::string err;
    CHECK(sp::validate_ops({shelf, gain}, err));
    CHECK_FALSE(sp::validate_ops({gain, shelf}, err)); // gain_to_lufs must be last (§6.2)
    CHECK(err.find("gain_to_lufs") != std::string::npos);

    // The JSON parser enforces the same rule.
    const std::string bad = R"({
      "recipe_version": 1,
      "ops": [
        { "op": "gain_to_lufs", "target_lufs": -20.0 },
        { "op": "high_shelf", "freq_hz": 4000, "gain_db": -6.0 }
      ]
    })";
    std::string parse_err;
    CHECK_FALSE(sp::recipe_from_json(bad, parse_err).has_value());
}
