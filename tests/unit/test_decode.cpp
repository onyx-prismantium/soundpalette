#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>

#include "soundpalette/audio.h"

namespace {

std::filesystem::path fixtures_dir() {
    const char *env = std::getenv("SP_FIXTURES_DIR");
    return env != nullptr ? std::filesystem::path(env)
                          : std::filesystem::path("tests/golden/fixtures");
}

double rms(const std::vector<float> &samples) {
    if (samples.empty()) {
        return 0.0;
    }
    double sum_sq = 0.0;
    for (float s : samples) {
        sum_sq += static_cast<double>(s) * static_cast<double>(s);
    }
    return std::sqrt(sum_sq / static_cast<double>(samples.size()));
}

} // namespace

TEST_CASE("decode/duration") {
    std::string err;
    auto buf = sp::decode_file(fixtures_dir() / "sine440_1s.wav", err);
    REQUIRE_MESSAGE(buf.has_value(), err);
    CHECK(std::fabs(buf->duration_s - 1.000) <= 0.002);
    CHECK(buf->src_rate == 48000);
    CHECK(buf->src_channels == 1);
    CHECK(buf->samples48k_mono.size() == 48000);
}

TEST_CASE("decode/lossy") {
    std::string wav_err;
    auto wav = sp::decode_file(fixtures_dir() / "sine440_1s.wav", wav_err);
    REQUIRE_MESSAGE(wav.has_value(), wav_err);
    const double wav_rms_db = 20.0 * std::log10(std::max(rms(wav->samples48k_mono), 1e-12));

    for (const char *name : {"sine440_1s.flac", "sine440_1s.ogg", "sine440_1s.mp3"}) {
        std::string err;
        auto lossy = sp::decode_file(fixtures_dir() / name, err);
        INFO(name, ": ", err);
        REQUIRE(lossy.has_value());
        const double lossy_rms_db = 20.0 * std::log10(std::max(rms(lossy->samples48k_mono), 1e-12));
        CHECK(std::fabs(lossy_rms_db - wav_rms_db) <= 1.0);
    }
}

TEST_CASE("loudness/calibration") {
    std::string err;
    auto buf = sp::decode_file(fixtures_dir() / "sine997_cal.wav", err);
    REQUIRE_MESSAGE(buf.has_value(), err);
    sp::Loudness loudness = sp::measure_loudness(*buf);
    CHECK(std::fabs(loudness.lufs_i - (-23.0)) <= 0.5);
}
