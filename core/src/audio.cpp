#include "soundpalette/audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "miniaudio.h"

#include "ebur128.h"

namespace sp {

namespace {

constexpr ma_uint32 kTargetSampleRate = 48000;
constexpr ma_uint32 kTargetChannels = 1;
constexpr double kMaxAnalysisSeconds = 30.0;
constexpr ma_uint64 kReadChunkFrames = 4096;

// stb_vorbis's push-mode backend can't report stream length (miniaudio's own docs on
// ma_decoder_get_length_in_pcm_frames), so source metadata is queried with a throwaway
// decoder configured to pass the internal format straight through (§6 decode step).
bool query_source_format(const std::filesystem::path &path, int &src_channels, int &src_rate,
                         std::string &err) {
    ma_decoder_config cfg = ma_decoder_config_init_default();
    ma_decoder decoder;
    ma_result result = ma_decoder_init_file(path.string().c_str(), &cfg, &decoder);
    if (result != MA_SUCCESS) {
        err = ma_result_description(result);
        return false;
    }

    ma_format format;
    ma_uint32 channels = 0;
    ma_uint32 sampleRate = 0;
    ma_decoder_get_data_format(&decoder, &format, &channels, &sampleRate, nullptr, 0);
    src_channels = static_cast<int>(channels);
    src_rate = static_cast<int>(sampleRate);

    ma_decoder_uninit(&decoder);
    return true;
}

} // namespace

std::optional<AudioBuffer> decode_file(const std::filesystem::path &path, std::string &err) {
    int src_channels = 0;
    int src_rate = 0;
    if (!query_source_format(path, src_channels, src_rate, err)) {
        return std::nullopt;
    }

    ma_decoder_config cfg =
        ma_decoder_config_init(ma_format_f32, kTargetChannels, kTargetSampleRate);
    ma_decoder decoder;
    ma_result result = ma_decoder_init_file(path.string().c_str(), &cfg, &decoder);
    if (result != MA_SUCCESS) {
        err = ma_result_description(result);
        return std::nullopt;
    }

    std::vector<float> samples;
    samples.reserve(static_cast<std::size_t>(kTargetSampleRate * 2));
    float chunk[kReadChunkFrames];
    for (;;) {
        ma_uint64 framesRead = 0;
        result = ma_decoder_read_pcm_frames(&decoder, chunk, kReadChunkFrames, &framesRead);
        if (framesRead > 0) {
            samples.insert(samples.end(), chunk, chunk + framesRead);
        }
        if (result != MA_SUCCESS || framesRead < kReadChunkFrames) {
            break;
        }
    }
    ma_decoder_uninit(&decoder);

    AudioBuffer buffer;
    buffer.src_rate = src_rate;
    buffer.src_channels = src_channels;
    buffer.duration_s =
        static_cast<double>(samples.size()) / static_cast<double>(kTargetSampleRate);

    const std::size_t maxFrames = static_cast<std::size_t>(kMaxAnalysisSeconds * kTargetSampleRate);
    if (samples.size() > maxFrames) {
        buffer.truncated = true;
        samples.resize(maxFrames);
    }
    buffer.samples48k_mono = std::move(samples);

    return buffer;
}

Loudness measure_loudness(const AudioBuffer &buffer) {
    Loudness loudness;

    ebur128_state *state =
        ebur128_init(kTargetChannels, kTargetSampleRate, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);

    double abs_peak = 0.0;
    for (float sample : buffer.samples48k_mono) {
        abs_peak = std::max(abs_peak, static_cast<double>(std::fabs(sample)));
    }

    double lufs_i = -std::numeric_limits<double>::infinity();
    double true_peak_linear = 0.0;
    if (state != nullptr) {
        if (!buffer.samples48k_mono.empty()) {
            // EBU R128 integrated loudness needs one full 400 ms gating block; shorter files
            // (real UI ticks are ~100 ms) would read -inf and be misclassified as silent.
            // Zero-padding the measurement input to 400 ms fixes that without biasing the
            // gated integration; every input >= 0.4 s is byte-for-byte unaffected.
            constexpr std::size_t kMinFrames = kTargetSampleRate * 2 / 5; // 400 ms
            if (buffer.samples48k_mono.size() < kMinFrames) {
                std::vector<float> padded(buffer.samples48k_mono);
                padded.resize(kMinFrames, 0.0f);
                ebur128_add_frames_float(state, padded.data(), padded.size());
            } else {
                ebur128_add_frames_float(state, buffer.samples48k_mono.data(),
                                         buffer.samples48k_mono.size());
            }
        }
        ebur128_loudness_global(state, &lufs_i);
        ebur128_true_peak(state, 0, &true_peak_linear);
        ebur128_destroy(&state);
    }

    loudness.lufs_i = lufs_i;
    loudness.true_peak_db = 20.0 * std::log10(std::max(true_peak_linear, 1e-12));
    loudness.silent = !std::isfinite(lufs_i) || abs_peak < 1e-4;

    return loudness;
}

} // namespace sp
