#include "soundpalette/features.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "kiss_fftr.h"

namespace sp {

namespace {

constexpr int kFftSize = 2048;
constexpr int kHop = 512;
constexpr int kNumBins = kFftSize / 2 + 1;
constexpr double kSampleRate = 48000.0;
constexpr double kPowerFloor = 1e-12;
constexpr double kFrameSkipPower = 1e-9;
constexpr double kPi = 3.14159265358979323846;

// [lo_hz, hi_hz) band edges, matching §6's six perceptual bands.
constexpr std::array<std::pair<double, double>, 6> kBandEdges{{
    {20.0, 80.0}, {80.0, 250.0}, {250.0, 600.0}, {600.0, 2500.0}, {2500.0, 8000.0}, {8000.0, 20000.0}
}};

double bin_freq(int k) {
    return static_cast<double>(k) * kSampleRate / kFftSize;
}

std::vector<double> hann_window(int n) {
    std::vector<double> w(n);
    for (int i = 0; i < n; ++i) {
        w[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (n - 1));
    }
    return w;
}

double gain_for_target_lufs(double lufs_i, double target_lufs) {
    return std::pow(10.0, (target_lufs - lufs_i) / 20.0);
}

std::vector<float> normalize_copy(const AudioBuffer& buffer, const Loudness& loudness) {
    const double gain = gain_for_target_lufs(loudness.lufs_i, -23.0);
    std::vector<float> out(buffer.samples48k_mono.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>(buffer.samples48k_mono[i] * gain);
    }
    return out;
}

struct FrameSpectrum {
    std::vector<double> power; // size kNumBins, floored at kPowerFloor
    double total_power_full = 0.0;   // sum over all bins 0..N/2
    double total_power_20_20k = 0.0; // sum over bins with f[k] in [20, 20000] Hz
};

// One pass computing all STFT-derived features together so we only run KissFFT once.
struct SpectralAccum {
    double centroid_sum = 0.0;
    double rolloff_sum = 0.0;
    double flatness_sum = 0.0;
    std::array<double, 6> band_sum{};
    double roughness_sum = 0.0;
    int roughness_count = 0;
    int valid_frames = 0;
};

FrameSpectrum compute_frame_spectrum(kiss_fftr_cfg cfg, const std::vector<double>& window,
                                      const float* frame_start) {
    std::vector<kiss_fft_scalar> in(kFftSize);
    for (int i = 0; i < kFftSize; ++i) {
        in[i] = static_cast<kiss_fft_scalar>(frame_start[i] * window[i]);
    }
    std::vector<kiss_fft_cpx> out(kNumBins);
    kiss_fftr(cfg, in.data(), out.data());

    FrameSpectrum spec;
    spec.power.resize(kNumBins);
    for (int k = 0; k < kNumBins; ++k) {
        double p = static_cast<double>(out[k].r) * out[k].r + static_cast<double>(out[k].i) * out[k].i;
        p = std::max(p, kPowerFloor);
        spec.power[k] = p;
        spec.total_power_full += p;
        double f = bin_freq(k);
        if (f >= 20.0 && f <= 20000.0) {
            spec.total_power_20_20k += p;
        }
    }
    return spec;
}

void accumulate_frame(const FrameSpectrum& spec, SpectralAccum& acc) {
    // centroid_hz, rolloff85_hz averaged over the full 0..Nyquist spectrum (§6 gives no band
    // restriction for these two; the 20 Hz-20 kHz restriction is explicit only for flatness).
    double weighted_freq = 0.0;
    for (int k = 0; k < kNumBins; ++k) {
        weighted_freq += bin_freq(k) * spec.power[k];
    }
    acc.centroid_sum += weighted_freq / spec.total_power_full;

    double cumulative = 0.0;
    double rolloff = bin_freq(kNumBins - 1);
    const double target = 0.85 * spec.total_power_full;
    for (int k = 0; k < kNumBins; ++k) {
        cumulative += spec.power[k];
        if (cumulative >= target) {
            rolloff = bin_freq(k);
            break;
        }
    }
    acc.rolloff_sum += rolloff;

    // flatness: geometric mean / arithmetic mean over 20 Hz-20 kHz bins.
    double log_sum = 0.0;
    double lin_sum = 0.0;
    int band_bin_count = 0;
    for (int k = 0; k < kNumBins; ++k) {
        double f = bin_freq(k);
        if (f < 20.0 || f > 20000.0) {
            continue;
        }
        log_sum += std::log(spec.power[k]);
        lin_sum += spec.power[k];
        ++band_bin_count;
    }
    if (band_bin_count > 0 && lin_sum > 0.0) {
        double geo_mean = std::exp(log_sum / band_bin_count);
        double arith_mean = lin_sum / band_bin_count;
        acc.flatness_sum += geo_mean / arith_mean;
    }

    // six perceptual bands, fraction of the 20 Hz-20 kHz total.
    for (std::size_t b = 0; b < kBandEdges.size(); ++b) {
        double lo = kBandEdges[b].first;
        double hi = kBandEdges[b].second;
        double band_power = 0.0;
        for (int k = 0; k < kNumBins; ++k) {
            double f = bin_freq(k);
            if (f >= lo && f < hi) {
                band_power += spec.power[k];
            }
        }
        if (spec.total_power_20_20k > 0.0) {
            acc.band_sum[b] += band_power / spec.total_power_20_20k;
        }
    }

    ++acc.valid_frames;
}

void accumulate_roughness(const FrameSpectrum& prev, const FrameSpectrum& cur, SpectralAccum& acc) {
    double flux_sum = 0.0;
    double norm_sum = 0.0;
    for (int k = 0; k < kNumBins; ++k) {
        double cur_sqrt = std::sqrt(cur.power[k]);
        double prev_sqrt = std::sqrt(prev.power[k]);
        flux_sum += std::max(0.0, cur_sqrt - prev_sqrt);
        norm_sum += cur_sqrt;
    }
    if (norm_sum > 0.0) {
        acc.roughness_sum += flux_sum / norm_sum;
        ++acc.roughness_count;
    }
}

double compute_zcr(const std::vector<float>& samples) {
    if (samples.size() < 2) {
        return 0.0;
    }
    std::uint64_t crossings = 0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        bool prev_neg = samples[i - 1] < 0.0f;
        bool cur_neg = samples[i] < 0.0f;
        if (prev_neg != cur_neg) {
            ++crossings;
        }
    }
    return static_cast<double>(crossings) / static_cast<double>(samples.size());
}

std::vector<double> envelope_follower(const std::vector<float>& samples, double time_constant_s) {
    std::vector<double> env(samples.size());
    const double alpha = std::exp(-1.0 / (kSampleRate * time_constant_s));
    double prev = 0.0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        double rectified = std::fabs(static_cast<double>(samples[i]));
        prev = alpha * prev + (1.0 - alpha) * rectified;
        env[i] = prev;
    }
    return env;
}

void compute_attack_tail(const std::vector<double>& env, double& attack_s, double& tail_s,
                          bool& tail_clipped) {
    attack_s = 0.0005;
    tail_s = 0.0;
    tail_clipped = false;
    if (env.empty()) {
        return;
    }

    std::size_t n_peak = 0;
    double peak = 0.0;
    for (std::size_t i = 0; i < env.size(); ++i) {
        if (env[i] > peak) {
            peak = env[i];
            n_peak = i;
        }
    }
    if (peak <= 0.0) {
        return;
    }

    const double attack_lo = 0.10 * peak;
    const double attack_hi = 0.90 * peak;
    std::size_t i_lo = env.size();
    for (std::size_t i = 0; i < env.size(); ++i) {
        if (env[i] >= attack_lo) {
            i_lo = i;
            break;
        }
    }
    std::size_t i_hi = env.size();
    if (i_lo < env.size()) {
        for (std::size_t i = i_lo; i < env.size(); ++i) {
            if (env[i] >= attack_hi) {
                i_hi = i;
                break;
            }
        }
    }
    if (i_lo < env.size() && i_hi < env.size() && i_hi >= i_lo) {
        attack_s = std::max(static_cast<double>(i_hi - i_lo) / kSampleRate, 0.0005);
    }

    const double tail_threshold = peak * std::pow(10.0, -60.0 / 20.0);
    std::size_t last_above = n_peak;
    for (std::size_t i = n_peak; i < env.size(); ++i) {
        if (env[i] >= tail_threshold) {
            last_above = i;
        }
    }
    if (last_above == env.size() - 1) {
        tail_clipped = true;
        tail_s = static_cast<double>(env.size() - 1 - n_peak) / kSampleRate;
    } else {
        tail_clipped = false;
        tail_s = static_cast<double>(last_above - n_peak) / kSampleRate;
    }
}

} // namespace

Features extract_features(const AudioBuffer& buffer, const Loudness& loudness) {
    Features features;
    if (loudness.silent || buffer.samples48k_mono.empty()) {
        return features;
    }

    std::vector<float> normalized = normalize_copy(buffer, loudness);

    features.zcr = compute_zcr(buffer.samples48k_mono);

    std::vector<double> env = envelope_follower(normalized, 0.005);
    compute_attack_tail(env, features.attack_s, features.tail_s, features.tail_clipped);

    // Zero-pad so at least one STFT frame exists even for very short clips.
    std::vector<float> padded = normalized;
    if (static_cast<int>(padded.size()) < kFftSize) {
        padded.resize(kFftSize, 0.0f);
    }

    const std::vector<double> window = hann_window(kFftSize);
    std::size_t lenmem = 0;
    kiss_fftr_alloc(kFftSize, 0, nullptr, &lenmem);
    std::vector<unsigned char> mem(lenmem);
    kiss_fftr_cfg cfg = kiss_fftr_alloc(kFftSize, 0, mem.data(), &lenmem);

    const int num_frames = static_cast<int>((padded.size() - kFftSize) / kHop) + 1;

    SpectralAccum acc;
    bool have_prev = false;
    FrameSpectrum prev_spec;
    for (int f = 0; f < num_frames; ++f) {
        const float* frame_start = padded.data() + static_cast<std::size_t>(f) * kHop;
        FrameSpectrum spec = compute_frame_spectrum(cfg, window, frame_start);

        if (spec.total_power_full >= kFrameSkipPower) {
            accumulate_frame(spec, acc);
            if (have_prev) {
                accumulate_roughness(prev_spec, spec, acc);
            }
            prev_spec = std::move(spec);
            have_prev = true;
        }
    }

    if (acc.valid_frames > 0) {
        features.centroid_hz = acc.centroid_sum / acc.valid_frames;
        features.rolloff85_hz = acc.rolloff_sum / acc.valid_frames;
        features.flatness = acc.flatness_sum / acc.valid_frames;
        for (std::size_t b = 0; b < features.bands.size(); ++b) {
            features.bands[b] = acc.band_sum[b] / acc.valid_frames;
        }
        features.warmth = features.bands[1] + features.bands[2];
    }
    if (acc.roughness_count > 0) {
        features.roughness = acc.roughness_sum / acc.roughness_count;
    }

    return features;
}

} // namespace sp
