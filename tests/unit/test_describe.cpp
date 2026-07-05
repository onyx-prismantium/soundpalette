#include <doctest/doctest.h>

#include <algorithm>

#include "soundpalette/audio.h"
#include "soundpalette/describe.h"
#include "soundpalette/psycho.h"

namespace {

// Analyzes a fixture through the real pipeline (paths are relative to the repo root, the
// doctest working directory).
bool analyze(const char *path, sp::Features &features, sp::Loudness &loudness,
             sp::PsychoFeatures &psycho) {
    std::string err;
    auto buffer = sp::decode_file(path, err);
    if (!buffer.has_value()) {
        return false;
    }
    loudness = sp::measure_loudness(*buffer);
    features = sp::extract_features(*buffer, loudness);
    psycho = sp::compute_psycho(*buffer, sp::MappingConfig{});
    return true;
}

} // namespace

TEST_CASE("describe/deterministic") {
    sp::Features f;
    sp::Loudness l;
    sp::PsychoFeatures psy;
    REQUIRE(analyze("tests/golden/fixtures/sine440_1s.wav", f, l, psy));

    // Two calls on identical inputs -> identical strings (§7 extension table).
    std::string a = sp::describe_words(f, l, psy);
    std::string b = sp::describe_words(f, l, psy);
    CHECK(a == b);
    CHECK(a.find("very tonal") != std::string::npos);

    sp::Features nf;
    sp::Loudness nl;
    sp::PsychoFeatures npsy;
    REQUIRE(analyze("tests/golden/fixtures/noise_white_1s.wav", nf, nl, npsy));
    std::string n = sp::describe_words(nf, nl, npsy);
    CHECK(n.find("pure noise") != std::string::npos);
}

TEST_CASE("describe/silent and sentence shape") {
    sp::Features f;
    sp::Loudness l;
    l.silent = true;
    CHECK(sp::describe_words(f, l, sp::PsychoFeatures{}) == "silent.");

    sp::Loudness live;
    live.lufs_i = -20.0;
    live.silent = false;
    f.centroid_hz = 1000.0;
    f.flatness = 0.1;
    f.warmth = 0.4;
    f.attack_s = 0.01;
    f.tail_s = 0.5;
    f.roughness = 0.05;
    sp::PsychoFeatures lp;
    lp.sones_n5 = 9.0;
    lp.sharpness_acum = 1.5;
    std::string s = sp::describe_words(f, live, lp);
    // Template: "<b>, <w>, <t>; <a>, <tl>; <l>, <g>."
    CHECK(std::count(s.begin(), s.end(), ';') == 2);
    CHECK(std::count(s.begin(), s.end(), ',') == 5);
    CHECK(s.back() == '.');
}
