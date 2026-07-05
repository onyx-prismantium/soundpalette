// Shared modulation front-end for roughness (Daniel & Weber style) and fluctuation strength
// (Zwicker & Fastl model description): per-band envelope -> Welch modulation spectrum ->
// weighted modulation depth m_i, adjacent-band envelope correlation k_i to suppress
// uncorrelated (noise-driven) modulation, then R = C_R·Σ(m_i·k_i)² and F likewise with the
// weighting recentred at ~4 Hz.
//
// Honest simplifications vs the original Daniel & Weber formulation (documented in NOTES.md,
// sanctioned by extension-3 §5 "the tolerances are the arbiter"): the carrier decomposition
// is the loudness filterbank's 28 third-octave bands rather than 47 half-bark excitation
// channels, and the best-modulation-frequency weighting is a fixed resonance at 70 Hz
// (roughness) / 4 Hz (fluctuation) instead of the per-channel family of curves. The output
// constants are calibrated on the definitional anchors (1 kHz, 60 dB, 100 % AM at 70 Hz =
// 1 asper; at 4 Hz = 1 vacil), which is how the units themselves are defined.

#include "psycho_internal.h"

#include <algorithm>
#include <cmath>

#include "kiss_fftr.h"

namespace sp::psycho {

namespace {
constexpr double kPi = 3.14159265358979323846; // MSVC has no M_PI
} // namespace

namespace {

constexpr double kRoughBestHz = 70.0;
constexpr double kFluctBestHz = 4.0;
// Band contributes only when audible-ish: mean envelope above ~26 dB SPL-equivalent.
constexpr double kEnvFloorPa = 4e-4;

// Calibration to the definitional anchors (see file header): kRoughCal makes the 1 kHz,
// 60 dB, 100 % AM @ 70 Hz fixture exactly 1 asper; kFluctCal makes the same carrier AM @ 4 Hz
// exactly 1 vacil — which is how the units are defined. The unit tests re-assert both
// anchors end to end.
constexpr double kRoughCal = 0.349736;
constexpr double kFluctCal = 0.225485;

// Resonance weighting |H(f)| = 2x/(1+x²), x = f/best — unity at best, ~0.11 a decade away.
double mod_weight(double f, double best_hz) {
    if (f <= 0.0) {
        return 0.0;
    }
    const double x = f / best_hz;
    return 2.0 * x / (1.0 + x * x);
}

// Weighted AC modulation amplitude and DC of one band envelope via a Welch modulation
// spectrum (1 s Hann segments, 50 % overlap, zero-padded tail).
struct BandModulation {
    double dc = 0.0;
    double m_rough = 0.0;
    double m_fluct = 0.0;
};

BandModulation analyze_band(const std::vector<float> &env, double env_rate, kiss_fftr_cfg cfg,
                            const std::vector<double> &window, int nfft) {
    BandModulation out;
    if (env.empty()) {
        return out;
    }
    double dc = 0.0;
    for (float e : env) {
        dc += e;
    }
    dc /= static_cast<double>(env.size());
    out.dc = dc;
    if (dc < kEnvFloorPa) {
        return out;
    }

    const int bins = nfft / 2 + 1;
    std::vector<double> psum(static_cast<std::size_t>(bins), 0.0);
    std::vector<kiss_fft_scalar> in(static_cast<std::size_t>(nfft));
    std::vector<kiss_fft_cpx> spec(static_cast<std::size_t>(bins));
    double wsq = 0.0;
    for (double w : window) {
        wsq += w * w;
    }

    const std::size_t hop = static_cast<std::size_t>(nfft) / 2;
    std::size_t frames = 0;
    for (std::size_t start = 0; start == 0 || start + hop < env.size(); start += hop) {
        for (int i = 0; i < nfft; ++i) {
            const std::size_t idx = start + static_cast<std::size_t>(i);
            const double x = idx < env.size() ? static_cast<double>(env[idx]) - dc : 0.0;
            in[static_cast<std::size_t>(i)] =
                static_cast<kiss_fft_scalar>(x * window[static_cast<std::size_t>(i)]);
        }
        kiss_fftr(cfg, in.data(), spec.data());
        for (int b = 0; b < bins; ++b) {
            const double re = spec[static_cast<std::size_t>(b)].r;
            const double im = spec[static_cast<std::size_t>(b)].i;
            psum[static_cast<std::size_t>(b)] += re * re + im * im;
        }
        ++frames;
    }
    if (frames == 0) {
        return out;
    }

    // Mean-square envelope AC per bin, weighted; modulation depth m = sqrt(2·Σw²·P)/dc, i.e.
    // the equivalent sinusoidal modulation amplitude over the carrier's DC.
    const double scale = 2.0 / (static_cast<double>(frames) * wsq * nfft);
    const double bin_hz = env_rate / nfft;
    double acc_rough = 0.0, acc_fluct = 0.0;
    for (int b = 1; b < bins; ++b) {
        const double f = b * bin_hz;
        if (f > 300.0) {
            break;
        }
        const double p = psum[static_cast<std::size_t>(b)] * scale;
        const double wr = mod_weight(f, kRoughBestHz);
        const double wf = mod_weight(f, kFluctBestHz);
        acc_rough += p * wr * wr;
        acc_fluct += p * wf * wf;
    }
    out.m_rough = std::min(1.0, std::sqrt(2.0 * acc_rough) / dc);
    out.m_fluct = std::min(1.0, std::sqrt(2.0 * acc_fluct) / dc);
    return out;
}

// Pearson correlation of two equal-length envelope tracks, clamped to [0, 1].
double envelope_corr(const std::vector<float> &a, const std::vector<float> &b) {
    const std::size_t n = std::min(a.size(), b.size());
    if (n < 8) {
        return 0.0;
    }
    double ma = 0.0, mb = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double da = a[i] - ma, db = b[i] - mb;
        sab += da * db;
        saa += da * da;
        sbb += db * db;
    }
    if (saa <= 0.0 || sbb <= 0.0) {
        return 0.0;
    }
    return std::clamp(sab / std::sqrt(saa * sbb), 0.0, 1.0);
}

} // namespace

ModulationMetrics modulation_metrics(const TvAnalysis &tv) {
    ModulationMetrics out;
    if (tv.band_env.empty() || tv.band_env[0].empty()) {
        return out;
    }
    const int nfft = 1024; // ~0.68 s at 1.5 kHz -> 1.46 Hz bins, resolves 4 Hz and 70 Hz
    std::vector<double> window(static_cast<std::size_t>(nfft));
    for (int i = 0; i < nfft; ++i) {
        window[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (nfft - 1));
    }
    std::size_t lenmem = 0;
    kiss_fftr_alloc(nfft, 0, nullptr, &lenmem);
    std::vector<unsigned char> mem(lenmem);
    kiss_fftr_cfg cfg = kiss_fftr_alloc(nfft, 0, mem.data(), &lenmem);

    std::vector<BandModulation> mods(static_cast<std::size_t>(kNumThirdOct));
    for (int i = 0; i < kNumThirdOct; ++i) {
        mods[static_cast<std::size_t>(i)] =
            analyze_band(tv.band_env[static_cast<std::size_t>(i)], tv.env_rate, cfg, window, nfft);
    }

    double r_sum = 0.0, f_sum = 0.0;
    for (int i = 0; i < kNumThirdOct; ++i) {
        const BandModulation &m = mods[static_cast<std::size_t>(i)];
        if (m.dc < kEnvFloorPa) {
            continue;
        }
        // Adjacent-band envelope correlation (Daniel & Weber's k factors): correlated
        // modulation across bands is perceived; independent band noise is not.
        const double k_lo = i > 0 ? envelope_corr(tv.band_env[static_cast<std::size_t>(i - 1)],
                                                  tv.band_env[static_cast<std::size_t>(i)])
                                  : envelope_corr(tv.band_env[static_cast<std::size_t>(i)],
                                                  tv.band_env[static_cast<std::size_t>(i + 1)]);
        const double k_hi = i + 1 < kNumThirdOct
                                ? envelope_corr(tv.band_env[static_cast<std::size_t>(i)],
                                                tv.band_env[static_cast<std::size_t>(i + 1)])
                                : k_lo;
        const double kk = k_lo * k_hi;
        const double r_i = m.m_rough * kk;
        const double f_i = m.m_fluct * kk;
        r_sum += r_i * r_i;
        f_sum += f_i * f_i;
    }
    out.roughness_asper = kRoughCal * r_sum;
    out.fluctuation_vacil = kFluctCal * f_sum;
    return out;
}

} // namespace sp::psycho
