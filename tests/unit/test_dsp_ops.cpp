#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>

#include "soundpalette/recipe.h"

namespace {

sp::NativeAudio load(const char *path) {
    std::string err;
    auto audio = sp::decode_file_native(path, err);
    REQUIRE_MESSAGE(audio.has_value(), err);
    return std::move(*audio);
}

void analyze(const sp::NativeAudio &audio, sp::Loudness &loudness, sp::Features &features) {
    sp::PsychoFeatures psycho;
    sp::analyze_native(audio, loudness, features, psycho);
}

sp::Op make_shelf(sp::OpType type, double freq_hz, double gain_db) {
    sp::Op op;
    op.op = type;
    op.freq_hz = freq_hz;
    op.gain_db = gain_db;
    op.q = 0.707;
    return op;
}

} // namespace

TEST_CASE("dsp/gain_to_lufs") {
    sp::NativeAudio audio = load("tests/golden/fixtures/sine997_cal.wav"); // -23 LUFS

    SUBCASE("hits an unconstrained target within tolerance") {
        sp::Op op;
        op.op = sp::OpType::kGainToLufs;
        op.target_lufs = -18.0;
        op.tp_ceiling_db = -1.0;
        sp::ApplyReport report;
        sp::apply_chain(audio, {op}, report);

        sp::Loudness loudness;
        sp::Features features;
        analyze(audio, loudness, features);
        CHECK(loudness.lufs_i == doctest::Approx(-18.0).epsilon(0.017)); // +-0.3 LU
        CHECK_FALSE(report.limited_by_peak);
    }

    SUBCASE("caps the gain at the true-peak ceiling") {
        sp::Op op;
        op.op = sp::OpType::kGainToLufs;
        op.target_lufs = -2.0; // would need ~+21 dB; peak allows ~+19
        op.tp_ceiling_db = -1.0;
        sp::ApplyReport report;
        sp::apply_chain(audio, {op}, report);

        sp::Loudness loudness;
        sp::Features features;
        analyze(audio, loudness, features);
        CHECK(report.limited_by_peak);
        CHECK(loudness.true_peak_db <= -0.9);
    }
}

TEST_CASE("dsp/high_shelf") {
    sp::NativeAudio audio = load("tests/golden/fixtures/noise_white_1s.wav");
    sp::Loudness loudness;
    sp::Features features;
    analyze(audio, loudness, features);
    CHECK(features.centroid_hz > 10500); // flat spectrum baseline (~12 kHz)

    sp::ApplyReport report;
    sp::apply_chain(audio, {make_shelf(sp::OpType::kHighShelf, 4000.0, -12.0)}, report);
    analyze(audio, loudness, features);
    CHECK(features.centroid_hz < 8500);
}

TEST_CASE("dsp/low_shelf") {
    sp::NativeAudio audio = load("tests/golden/fixtures/noise_white_1s.wav");
    sp::Loudness loudness;
    sp::Features features;
    analyze(audio, loudness, features);
    const double warmth_before = features.warmth;

    sp::ApplyReport report;
    sp::apply_chain(audio, {make_shelf(sp::OpType::kLowShelf, 250.0, 6.0)}, report);
    analyze(audio, loudness, features);
    CHECK(features.warmth >= warmth_before + 0.010);
}

TEST_CASE("dsp/attack_soften") {
    sp::NativeAudio audio = load("tests/golden/fixtures/click.wav");
    sp::Op op;
    op.op = sp::OpType::kAttackSoften;
    op.fade_ms = 25.0;
    sp::ApplyReport report;
    sp::apply_chain(audio, {op}, report);

    sp::Loudness loudness;
    sp::Features features;
    analyze(audio, loudness, features);
    CHECK(features.attack_s >= 0.008);
    CHECK(features.attack_s <= 0.030);
}

TEST_CASE("dsp/tail_shorten") {
    SUBCASE("shortens toward the target") {
        sp::NativeAudio audio = load("tests/golden/fixtures/decay_t60.wav"); // tail ~0.5 s
        sp::Op op;
        op.op = sp::OpType::kTailShorten;
        op.target_tail_s = 0.25;
        sp::ApplyReport report;
        sp::apply_chain(audio, {op}, report);

        sp::Loudness loudness;
        sp::Features features;
        analyze(audio, loudness, features);
        CHECK(features.tail_s >= 0.20);
        CHECK(features.tail_s <= 0.30);
    }

    SUBCASE("no-op when the target exceeds the current tail") {
        sp::NativeAudio audio = load("tests/golden/fixtures/decay_t60.wav");
        const std::size_t frames_before = audio.frame_count();
        sp::Op op;
        op.op = sp::OpType::kTailShorten;
        op.target_tail_s = 5.0;
        sp::ApplyReport report;
        sp::apply_chain(audio, {op}, report);
        CHECK(audio.frame_count() == frames_before);
    }
}

TEST_CASE("dsp/channels_rate") {
    // A 44.1 kHz stereo derivative of the 440 Hz sine, generated in-test (§7).
    sp::NativeAudio stereo;
    stereo.rate = 44100;
    stereo.channels.resize(2);
    for (int c = 0; c < 2; ++c) {
        stereo.channels[static_cast<std::size_t>(c)].resize(44100);
        for (std::size_t i = 0; i < 44100; ++i) {
            const double t = static_cast<double>(i) / 44100.0;
            const double amp = c == 0 ? 0.5 : 0.25; // distinct channels
            stereo.channels[static_cast<std::size_t>(c)][i] =
                static_cast<float>(amp * std::sin(2.0 * 3.14159265358979323846 * 440.0 * t));
        }
    }

    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / "sp_test_stereo44k.wav";
    std::string err;
    REQUIRE_MESSAGE(sp::write_wav_f32(tmp, stereo, err), err);

    auto decoded = sp::decode_file_native(tmp, err);
    REQUIRE_MESSAGE(decoded.has_value(), err);
    CHECK(decoded->rate == 44100);
    CHECK(decoded->channels.size() == 2);

    sp::ApplyReport report;
    sp::apply_chain(*decoded, {make_shelf(sp::OpType::kLowShelf, 250.0, 3.0)}, report);
    CHECK(decoded->rate == 44100); // processed without resample (§6.1)
    CHECK(decoded->channels.size() == 2);
    CHECK(decoded->frame_count() == stereo.frame_count());

    std::filesystem::remove(tmp);
}
