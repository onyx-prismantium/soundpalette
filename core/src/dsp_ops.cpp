// Recipe-op DSP implementations (extension §6.1/§6.2). All processing on native-rate planar
// float audio; biquad coefficients and state in double; gains/envelopes linked across channels.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "miniaudio.h"

#include "ebur128.h"

#include "soundpalette/recipe.h"

namespace sp {

namespace {

constexpr double kPi = 3.14159265358979323846;

// 5 ms one-pole envelope of the channel-mean mono mix, same form as the §6 analysis envelope.
std::vector<double> mono_envelope(const NativeAudio &audio) {
    const std::size_t n = audio.frame_count();
    const std::size_t ch = audio.channels.size();
    std::vector<double> env(n);
    const double alpha = std::exp(-1.0 / (0.005 * audio.rate));
    double e = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        double mix = 0.0;
        for (std::size_t c = 0; c < ch; ++c) {
            mix += audio.channels[c][i];
        }
        mix = std::fabs(mix / static_cast<double>(ch));
        e = alpha * e + (1.0 - alpha) * mix;
        env[i] = e;
    }
    return env;
}

void apply_scalar_gain(NativeAudio &audio, double factor) {
    for (auto &channel : audio.channels) {
        for (float &x : channel) {
            x = static_cast<float>(x * factor);
        }
    }
}

// Integrated LUFS + true peak of the buffer at its native rate/channel count.
void measure_native(const NativeAudio &audio, double &lufs_i, double &true_peak_db) {
    const unsigned int ch = static_cast<unsigned int>(audio.channels.size());
    const std::size_t n = audio.frame_count();
    lufs_i = -HUGE_VAL;
    true_peak_db = -HUGE_VAL;

    ebur128_state *state = ebur128_init(ch, static_cast<unsigned long>(audio.rate),
                                        EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
    if (state == nullptr || n == 0) {
        if (state != nullptr) {
            ebur128_destroy(&state);
        }
        return;
    }
    std::vector<float> interleaved(n * ch);
    for (std::size_t i = 0; i < n; ++i) {
        for (unsigned int c = 0; c < ch; ++c) {
            interleaved[i * ch + c] = audio.channels[c][i];
        }
    }
    ebur128_add_frames_float(state, interleaved.data(), n);
    ebur128_loudness_global(state, &lufs_i);
    double tp_linear_max = 0.0;
    for (unsigned int c = 0; c < ch; ++c) {
        double tp = 0.0;
        ebur128_true_peak(state, c, &tp);
        tp_linear_max = std::max(tp_linear_max, tp);
    }
    true_peak_db = tp_linear_max > 0.0 ? 20.0 * std::log10(tp_linear_max) : -HUGE_VAL;
    ebur128_destroy(&state);
}

// gain_to_lufs (§6.2): scalar gain to the target; capped so true peak stays under the ceiling
// (no limiter in v2 — limited_by_peak is reported instead).
void op_gain_to_lufs(NativeAudio &audio, const Op &op, ApplyReport &report) {
    double lufs = 0.0, tp_db = 0.0;
    measure_native(audio, lufs, tp_db);
    if (!std::isfinite(lufs)) {
        return; // silent/immeasurable: no meaningful gain target
    }
    double gain_db = op.target_lufs - lufs;
    if (std::isfinite(tp_db) && tp_db + gain_db > op.tp_ceiling_db) {
        gain_db = op.tp_ceiling_db - tp_db;
        report.limited_by_peak = true;
    }
    apply_scalar_gain(audio, std::pow(10.0, gain_db / 20.0));
}

// RBJ Audio EQ Cookbook shelves (Q form): A = 10^(gain/40), w0 = 2*pi*f/fs,
// alpha = sin(w0)/(2Q). Behavioral gates in extension §7 guard the transcription.
struct Biquad {
    double b0, b1, b2, a1, a2; // normalized by a0
};

Biquad shelf_coefficients(OpType type, double freq_hz, double gain_db, double q, int rate) {
    const double a = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * kPi * freq_hz / rate;
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double sq = 2.0 * std::sqrt(a) * alpha;

    double b0, b1, b2, a0, a1, a2;
    if (type == OpType::kLowShelf) {
        b0 = a * ((a + 1.0) - (a - 1.0) * cw + sq);
        b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cw);
        b2 = a * ((a + 1.0) - (a - 1.0) * cw - sq);
        a0 = (a + 1.0) + (a - 1.0) * cw + sq;
        a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cw);
        a2 = (a + 1.0) + (a - 1.0) * cw - sq;
    } else {
        b0 = a * ((a + 1.0) + (a - 1.0) * cw + sq);
        b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cw);
        b2 = a * ((a + 1.0) + (a - 1.0) * cw - sq);
        a0 = (a + 1.0) - (a - 1.0) * cw + sq;
        a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cw);
        a2 = (a + 1.0) - (a - 1.0) * cw - sq;
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

void op_shelf(NativeAudio &audio, const Op &op) {
    Biquad c = shelf_coefficients(op.op, op.freq_hz, op.gain_db, op.q, audio.rate);
    for (auto &channel : audio.channels) {
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0; // state in double (§6.1)
        for (float &sample : channel) {
            const double x = sample;
            const double y = c.b0 * x + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            sample = static_cast<float>(y);
        }
    }
}

// attack_soften (§6.2): half-cosine ramp 0.5*(1-cos(pi*t/F)) starting at max(0, t_on - 2 ms),
// where t_on = first sample with the 5 ms envelope >= 0.10 * peak. Zero before the ramp.
void op_attack_soften(NativeAudio &audio, const Op &op) {
    const std::size_t n = audio.frame_count();
    if (n == 0 || op.fade_ms <= 0.0) {
        return;
    }
    std::vector<double> env = mono_envelope(audio);
    const double peak = *std::max_element(env.begin(), env.end());
    if (peak <= 0.0) {
        return;
    }
    std::size_t t_on = 0;
    while (t_on < n && env[t_on] < 0.10 * peak) {
        ++t_on;
    }
    const std::size_t back = static_cast<std::size_t>(0.002 * audio.rate);
    const std::size_t start = t_on > back ? t_on - back : 0;
    const std::size_t fade_len =
        std::max<std::size_t>(1, static_cast<std::size_t>(op.fade_ms / 1000.0 * audio.rate));

    for (auto &channel : audio.channels) {
        for (std::size_t i = 0; i < std::min(start + fade_len, n); ++i) {
            double factor = 0.0;
            if (i >= start) {
                const double t = static_cast<double>(i - start) / static_cast<double>(fade_len);
                factor = 0.5 * (1.0 - std::cos(kPi * t));
            }
            channel[i] = static_cast<float>(channel[i] * factor);
        }
    }
}

// tail_shorten (§6.2): extra exponential attenuation past the envelope peak at
// dr = 60*(1/T_target - 1/T_current) dB/s, then fade-out and trim below -80 dBFS.
void op_tail_shorten(NativeAudio &audio, const Op &op) {
    const std::size_t n = audio.frame_count();
    if (n == 0 || op.target_tail_s <= 0.0) {
        return;
    }
    std::vector<double> env = mono_envelope(audio);
    const std::size_t t_p =
        static_cast<std::size_t>(std::max_element(env.begin(), env.end()) - env.begin());
    const double peak = env[t_p];
    if (peak <= 0.0) {
        return;
    }
    const double floor60 = peak * 1e-3; // -60 dB, §6's T60-style tail measure
    std::size_t last = n - 1;
    while (last > t_p && env[last] < floor60) {
        --last;
    }
    const double t_current = static_cast<double>(last - t_p) / audio.rate;
    if (op.target_tail_s >= t_current || t_current <= 0.0) {
        return; // only acts when target < current (§6.2)
    }

    const double dr_db_per_s = 60.0 * (1.0 / op.target_tail_s - 1.0 / t_current);
    for (auto &channel : audio.channels) {
        for (std::size_t i = t_p + 1; i < n; ++i) {
            const double t = static_cast<double>(i - t_p) / audio.rate;
            channel[i] = static_cast<float>(channel[i] * std::pow(10.0, -dr_db_per_s * t / 20.0));
        }
    }

    // Trim: keep up to the last sample above -80 dBFS plus a 5 ms release fade, and make sure
    // the kept audio ends in a fade to zero (10 ms window or shorter).
    constexpr double kTrimFloor = 1e-4; // -80 dBFS
    std::size_t last_above = 0;
    for (std::size_t i = 0; i < n; ++i) {
        for (const auto &channel : audio.channels) {
            if (std::fabs(channel[i]) >= kTrimFloor) {
                last_above = i;
                break;
            }
        }
    }
    const std::size_t release = static_cast<std::size_t>(0.005 * audio.rate);
    const std::size_t end = std::min(n, last_above + release + 1);
    const std::size_t fade_len = std::min<std::size_t>(
        end, std::max<std::size_t>(release, static_cast<std::size_t>(0.010 * audio.rate)));
    const std::size_t fade_start = end - fade_len;
    // Keep at least 450 ms (digital silence after the fade): EBU R128 integrated loudness
    // needs a 400 ms gating block, and a shorter output would re-analyze as "silent" under
    // §6's rule (documented deviation from the bare trim clause, NOTES.md M9).
    const std::size_t keep_end =
        std::max(end, std::min(n, static_cast<std::size_t>(0.45 * audio.rate)));
    for (auto &channel : audio.channels) {
        for (std::size_t i = fade_start; i < end; ++i) {
            const double t =
                1.0 - static_cast<double>(i - fade_start) / static_cast<double>(fade_len);
            channel[i] = static_cast<float>(channel[i] * t);
        }
        for (std::size_t i = end; i < keep_end; ++i) {
            channel[i] = 0.0f;
        }
        channel.resize(keep_end);
    }
}

} // namespace

const char *op_name(OpType op) {
    switch (op) {
    case OpType::kGainDb:
        return "gain_db";
    case OpType::kGainToLufs:
        return "gain_to_lufs";
    case OpType::kLowShelf:
        return "low_shelf";
    case OpType::kHighShelf:
        return "high_shelf";
    case OpType::kAttackSoften:
        return "attack_soften";
    case OpType::kTailShorten:
        return "tail_shorten";
    }
    return "unknown";
}

std::optional<NativeAudio> decode_file_native(const std::filesystem::path &path, std::string &err) {
    ma_decoder decoder;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0); // native ch/rate
    if (ma_decoder_init_file(path.string().c_str(), &cfg, &decoder) != MA_SUCCESS) {
        err = "cannot decode " + path.string();
        return std::nullopt;
    }
    ma_format format = ma_format_unknown;
    ma_uint32 channels = 0, rate = 0;
    ma_decoder_get_data_format(&decoder, &format, &channels, &rate, nullptr, 0);
    if (channels == 0 || rate == 0) {
        ma_decoder_uninit(&decoder);
        err = "cannot query format of " + path.string();
        return std::nullopt;
    }

    NativeAudio audio;
    audio.rate = static_cast<int>(rate);
    audio.channels.resize(channels);

    std::vector<float> chunk(4096 * channels);
    for (;;) {
        ma_uint64 read = 0;
        ma_result r = ma_decoder_read_pcm_frames(&decoder, chunk.data(), 4096, &read);
        for (ma_uint64 i = 0; i < read; ++i) {
            for (ma_uint32 c = 0; c < channels; ++c) {
                audio.channels[c].push_back(chunk[i * channels + c]);
            }
        }
        if (r != MA_SUCCESS || read < 4096) {
            break;
        }
    }
    ma_decoder_uninit(&decoder);

    if (audio.frame_count() == 0) {
        err = "no audio frames in " + path.string();
        return std::nullopt;
    }
    return audio;
}

bool write_wav_f32(const std::filesystem::path &path, const NativeAudio &audio, std::string &err) {
    const std::size_t ch = audio.channels.size();
    const std::size_t n = audio.frame_count();
    ma_encoder encoder;
    ma_encoder_config cfg =
        ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, static_cast<ma_uint32>(ch),
                               static_cast<ma_uint32>(audio.rate));
    if (ma_encoder_init_file(path.string().c_str(), &cfg, &encoder) != MA_SUCCESS) {
        err = "cannot write " + path.string();
        return false;
    }
    std::vector<float> interleaved(n * ch);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t c = 0; c < ch; ++c) {
            interleaved[i * ch + c] = audio.channels[c][i];
        }
    }
    ma_uint64 written = 0;
    ma_encoder_write_pcm_frames(&encoder, interleaved.data(), n, &written);
    ma_encoder_uninit(&encoder);
    if (written != n) {
        err = "short write to " + path.string();
        return false;
    }
    return true;
}

void apply_chain(NativeAudio &audio, const std::vector<Op> &ops, ApplyReport &report) {
    for (const Op &op : ops) {
        switch (op.op) {
        case OpType::kGainDb:
            apply_scalar_gain(audio, std::pow(10.0, op.db / 20.0));
            break;
        case OpType::kGainToLufs:
            op_gain_to_lufs(audio, op, report);
            break;
        case OpType::kLowShelf:
        case OpType::kHighShelf:
            op_shelf(audio, op);
            break;
        case OpType::kAttackSoften:
            op_attack_soften(audio, op);
            break;
        case OpType::kTailShorten:
            op_tail_shorten(audio, op);
            break;
        }
    }
}

void analyze_native(const NativeAudio &audio, Loudness &loudness, Features &features) {
    // Downmix (channel mean) then resample to 48 kHz with miniaudio's default resampler —
    // the same preprocessing a scan of the written file goes through (§6.1: re-analyze via
    // the standard pipeline).
    const std::size_t n = audio.frame_count();
    const std::size_t ch = audio.channels.size();
    std::vector<float> mono(n);
    for (std::size_t i = 0; i < n; ++i) {
        double mix = 0.0;
        for (std::size_t c = 0; c < ch; ++c) {
            mix += audio.channels[c][i];
        }
        mono[i] = static_cast<float>(mix / static_cast<double>(ch));
    }

    AudioBuffer buffer;
    buffer.src_rate = audio.rate;
    buffer.src_channels = static_cast<int>(ch);
    buffer.duration_s = static_cast<double>(n) / audio.rate;

    if (audio.rate == 48000) {
        buffer.samples48k_mono = std::move(mono);
    } else {
        ma_resampler_config cfg =
            ma_resampler_config_init(ma_format_f32, 1, static_cast<ma_uint32>(audio.rate), 48000,
                                     ma_resample_algorithm_linear);
        ma_resampler resampler;
        if (ma_resampler_init(&cfg, nullptr, &resampler) == MA_SUCCESS) {
            ma_uint64 out_cap = 0;
            ma_resampler_get_expected_output_frame_count(&resampler, n, &out_cap);
            buffer.samples48k_mono.resize(out_cap + 16);
            ma_uint64 in_frames = n;
            ma_uint64 out_frames = buffer.samples48k_mono.size();
            ma_resampler_process_pcm_frames(&resampler, mono.data(), &in_frames,
                                            buffer.samples48k_mono.data(), &out_frames);
            buffer.samples48k_mono.resize(out_frames);
            ma_resampler_uninit(&resampler, nullptr);
        } else {
            buffer.samples48k_mono = std::move(mono); // degenerate fallback
        }
    }

    constexpr std::size_t kCap = 48000u * 30u; // §6: analyze at most the first 30 s
    if (buffer.samples48k_mono.size() > kCap) {
        buffer.samples48k_mono.resize(kCap);
        buffer.truncated = true;
    }

    loudness = measure_loudness(buffer);
    features = extract_features(buffer, loudness);
}

} // namespace sp
