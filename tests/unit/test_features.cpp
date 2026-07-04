#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>

#include "soundpalette/audio.h"
#include "soundpalette/features.h"

namespace {

std::filesystem::path fixtures_dir() {
    const char *env = std::getenv("SP_FIXTURES_DIR");
    return env != nullptr ? std::filesystem::path(env)
                          : std::filesystem::path("tests/golden/fixtures");
}

struct Analyzed {
    sp::AudioBuffer buffer;
    sp::Loudness loudness;
    sp::Features features;
};

Analyzed analyze(const char *name) {
    std::string err;
    auto buf = sp::decode_file(fixtures_dir() / name, err);
    REQUIRE_MESSAGE(buf.has_value(), name << ": " << err);
    Analyzed a;
    a.buffer = std::move(*buf);
    a.loudness = sp::measure_loudness(a.buffer);
    a.features = sp::extract_features(a.buffer, a.loudness);
    return a;
}

} // namespace

TEST_CASE("features/centroid_sine") {
    Analyzed a = analyze("sine440_1s.wav");
    CHECK(a.features.centroid_hz >= 410.0);
    CHECK(a.features.centroid_hz <= 470.0);
}

TEST_CASE("features/centroid_noise") {
    Analyzed a = analyze("noise_white_1s.wav");
    CHECK(a.features.centroid_hz >= 10500.0);
    CHECK(a.features.centroid_hz <= 13500.0);
}

TEST_CASE("features/flatness") {
    Analyzed sine = analyze("sine440_1s.wav");
    CHECK(sine.features.flatness < 0.05);

    Analyzed noise = analyze("noise_white_1s.wav");
    CHECK(noise.features.flatness > 0.45);
}

TEST_CASE("features/zcr") {
    Analyzed a = analyze("sine440_1s.wav");
    const double expected = 2.0 * 440.0 / 48000.0;
    CHECK(std::fabs(a.features.zcr - expected) <= 0.10 * expected);
}

TEST_CASE("features/attack") {
    Analyzed click = analyze("click.wav");
    CHECK(click.features.attack_s < 0.010);

    Analyzed decay = analyze("decay_t60.wav");
    CHECK(decay.features.attack_s < 0.012);
}

TEST_CASE("features/tail") {
    Analyzed a = analyze("decay_t60.wav");
    CHECK(a.features.tail_s >= 0.40);
    CHECK(a.features.tail_s <= 0.60);
}
