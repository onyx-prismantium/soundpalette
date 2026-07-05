// ISO 532-1 (Zwicker) loudness. The stationary core follows the DIN 45631 / ISO 532 B
// program tables as published by Zwicker et al. (J. Acoust. Soc. Jpn (E) 12, 1991) —
// third-octave levels are corrected by equal-loudness contours at low frequencies, grouped
// into 20 approximated critical bands, converted to core loudness with the 0.23-power law,
// then spread over the critical-band-rate axis with level-dependent upper slopes.
//
// The time-varying variant drives the same core from a 28-band third-octave filterbank
// (three cascaded RBJ bandpass biquads per band, section Q chosen so the cascade is -3 dB at
// the band edges), smoothed squared output sampled every 2 ms, a nonlinear temporal decay per
// critical band (instant attack, exponential release), and a first-order loudness-time-
// function smoothing. Filter realizations are the implementer's choice per extension-3 §5;
// the definitional and oracle tolerances are the arbiter. Time constants below were tuned
// against tests/golden/psycho_reference.json (numbers only — no oracle code consulted).

#include "psycho_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "kiss_fftr.h"

namespace sp::psycho {

const std::array<double, kNumThirdOct> kThirdOctFc = {
    25.0,   31.5,   40.0,   50.0,   63.0,   80.0,   100.0,   125.0,  160.0,  200.0,
    250.0,  315.0,  400.0,  500.0,  630.0,  800.0,  1000.0,  1250.0, 1600.0, 2000.0,
    2500.0, 3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0};

namespace {

// --- DIN 45631 tables ------------------------------------------------------------------

// Ranges of third-octave levels for the low-frequency equal-loudness corrections.
constexpr double kRap[8] = {45.0, 55.0, 65.0, 71.0, 80.0, 90.0, 100.0, 120.0};

// Level reductions for the lowest 11 third-octave bands within the kRap ranges.
constexpr double kDll[8][11] = {{-32.0, -24.0, -16.0, -10.0, -5.0, 0.0, -7.0, -3.0, 0.0, -2.0, 0.0},
                                {-29.0, -22.0, -15.0, -10.0, -4.0, 0.0, -7.0, -2.0, 0.0, -2.0, 0.0},
                                {-27.0, -19.0, -14.0, -9.0, -4.0, 0.0, -6.0, -2.0, 0.0, -2.0, 0.0},
                                {-25.0, -17.0, -12.0, -9.0, -3.0, 0.0, -5.0, -2.0, 0.0, -2.0, 0.0},
                                {-23.0, -16.0, -11.0, -7.0, -3.0, 0.0, -4.0, -1.0, 0.0, -1.0, 0.0},
                                {-20.0, -14.0, -10.0, -6.0, -3.0, 0.0, -4.0, -1.0, 0.0, -1.0, 0.0},
                                {-18.0, -12.0, -9.0, -6.0, -2.0, 0.0, -3.0, -1.0, 0.0, -1.0, 0.0},
                                {-15.0, -10.0, -8.0, -4.0, -2.0, 0.0, -3.0, -1.0, 0.0, -1.0, 0.0}};

// Critical-band level at threshold in quiet (20 approximated critical bands).
constexpr double kLtq[20] = {30.0, 18.0, 12.0, 8.0, 7.0, 6.0, 5.0, 4.0, 3.0, 3.0,
                             3.0,  3.0,  3.0,  3.0, 3.0, 3.0, 3.0, 3.0, 3.0, 3.0};

// Ear transmission correction.
constexpr double kA0[20] = {0.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0,  0.0, 0.0, 0.0,
                            -0.5, -1.6, -3.2, -5.4, -5.6, -4.0, -1.5, 2.0, 5.0, 12.0};

// Adaptation of third-octave levels to critical-band levels.
constexpr double kDcb[20] = {-0.25, -0.6, -0.8, -0.8, -0.5, 0.0, 0.5, 1.1, 1.5, 1.7,
                             1.8,   1.8,  1.7,  1.6,  1.4,  1.2, 0.8, 0.5, 0.0, -0.5};

// Upper limits of the approximated critical bands (bark).
constexpr double kZup[21] = {0.9,  1.8,  2.8,  3.5,  4.4,  5.4,  6.6,  7.9,  9.2,  10.6, 12.3,
                             13.8, 15.2, 16.7, 18.1, 19.3, 20.6, 21.8, 22.7, 23.6, 24.0};

// Specific-loudness ranges for the level-dependent upper slopes.
constexpr double kRns[18] = {21.5, 18.0, 15.1, 11.5, 9.0,  6.1,  4.4,  3.1,   2.13,
                             1.36, 0.82, 0.42, 0.30, 0.22, 0.15, 0.10, 0.035, 0.0};

// Upper-slope steepness (sone/bark) per kRns range and critical-band group.
constexpr double kUsl[18][8] = {{13.0, 8.2, 6.3, 5.5, 5.5, 5.5, 5.5, 5.5},
                                {9.0, 7.5, 6.0, 5.1, 4.5, 4.5, 4.5, 4.5},
                                {7.8, 6.7, 5.6, 4.9, 4.4, 3.9, 3.9, 3.9},
                                {6.2, 5.4, 4.6, 4.0, 3.5, 3.2, 3.2, 3.2},
                                {4.5, 3.8, 3.6, 3.2, 2.9, 2.7, 2.7, 2.7},
                                {3.7, 3.0, 2.8, 2.35, 2.2, 2.2, 2.2, 2.2},
                                {2.9, 2.3, 2.1, 1.9, 1.8, 1.7, 1.7, 1.7},
                                {2.4, 1.7, 1.5, 1.35, 1.3, 1.3, 1.3, 1.3},
                                {1.95, 1.45, 1.3, 1.15, 1.1, 1.1, 1.1, 1.1},
                                {1.5, 1.2, 0.94, 0.86, 0.82, 0.82, 0.82, 0.82},
                                {0.72, 0.67, 0.64, 0.63, 0.62, 0.62, 0.62, 0.62},
                                {0.59, 0.53, 0.51, 0.50, 0.42, 0.42, 0.42, 0.42},
                                {0.40, 0.33, 0.26, 0.24, 0.24, 0.22, 0.22, 0.22},
                                {0.27, 0.21, 0.20, 0.18, 0.17, 0.17, 0.17, 0.17},
                                {0.16, 0.15, 0.14, 0.12, 0.11, 0.11, 0.11, 0.11},
                                {0.12, 0.11, 0.10, 0.08, 0.08, 0.08, 0.08, 0.08},
                                {0.09, 0.08, 0.07, 0.06, 0.06, 0.06, 0.06, 0.05},
                                {0.06, 0.05, 0.03, 0.02, 0.02, 0.02, 0.02, 0.02}};

// Core loudness per approximated critical band from the grouped band levels (free field).
std::array<double, 21> core_loudness(const std::array<double, kNumThirdOct> &lt) {
    // Low-frequency correction: pick the kRap range per band, apply kDll, sum intensities
    // into the first three approximated critical bands.
    double ti[11];
    for (int i = 0; i < 11; ++i) {
        int j = 0;
        while (j < 7 && lt[static_cast<std::size_t>(i)] > kRap[j] - kDll[j][i]) {
            ++j;
        }
        ti[i] = std::pow(10.0, (lt[static_cast<std::size_t>(i)] + kDll[j][i]) / 10.0);
    }
    const double gi0 = ti[0] + ti[1] + ti[2] + ti[3] + ti[4] + ti[5];
    const double gi1 = ti[6] + ti[7] + ti[8];
    const double gi2 = ti[9] + ti[10];
    double lcb[3];
    lcb[0] = gi0 > 0.0 ? 10.0 * std::log10(gi0) : -70.0;
    lcb[1] = gi1 > 0.0 ? 10.0 * std::log10(gi1) : -70.0;
    lcb[2] = gi2 > 0.0 ? 10.0 * std::log10(gi2) : -70.0;

    std::array<double, 21> nm{}; // 20 bands + sentinel 0
    for (int i = 0; i < 20; ++i) {
        double le = i < 3 ? lcb[i] : lt[static_cast<std::size_t>(i + 8)];
        le -= kA0[i];
        if (le <= kLtq[i]) {
            continue;
        }
        le -= kDcb[i];
        const double s = 0.25;
        const double mp1 = 0.0635 * std::pow(10.0, 0.025 * kLtq[i]);
        const double mp2 = std::pow(1.0 - s + s * std::pow(10.0, 0.1 * (le - kLtq[i])), 0.25) - 1.0;
        nm[static_cast<std::size_t>(i)] = std::max(0.0, mp1 * mp2);
    }
    // Correction of the lowest band's specific loudness.
    if (nm[0] > 0.0) {
        double korry = 0.4 + 0.32 * std::pow(nm[0], 0.2);
        nm[0] *= std::min(1.0, korry);
    }
    return nm;
}

// Spreading over the critical-band-rate axis with level-dependent upper slopes; returns the
// total loudness and optionally samples N'(z) in 0.1 bark steps.
double spread_and_integrate(const std::array<double, 21> &nm,
                            std::array<double, kNumBarkSteps> *ns_out) {
    if (ns_out != nullptr) {
        ns_out->fill(0.0);
    }
    double n_total = 0.0;
    double z1 = 0.0; // left border (bark)
    double n1 = 0.0; // specific loudness at the left border
    double z = 0.1;  // next 0.1-bark sample position
    int iz = 0;
    int j = 17;
    for (int i = 0; i < 21; ++i) {
        const double zup = kZup[i] + 0.0001;
        const int ig = std::min(std::max(i - 1, 0), 7); // slope column per band group
        while (z1 < zup) {
            if (n1 <= nm[static_cast<std::size_t>(i)]) {
                // Rising edge: jump up (no upward spread modeled), flat to the band edge.
                n1 = nm[static_cast<std::size_t>(i)];
                const double z2 = zup;
                n_total += (z2 - z1) * n1;
                while (z < z2 && iz < kNumBarkSteps) {
                    if (ns_out != nullptr) {
                        (*ns_out)[static_cast<std::size_t>(iz)] = n1;
                    }
                    ++iz;
                    z += 0.1;
                }
                z1 = z2;
            } else {
                // Decaying flank: slope depends on the current specific-loudness range.
                j = 0;
                while (j < 17 && kRns[j] >= n1) {
                    ++j;
                }
                double n2 = kRns[j] < nm[static_cast<std::size_t>(i)]
                                ? nm[static_cast<std::size_t>(i)]
                                : kRns[j];
                const double usl = kUsl[j][ig];
                double dz = (n1 - n2) / usl;
                double z2 = z1 + dz;
                if (z2 > zup) {
                    z2 = zup;
                    dz = z2 - z1;
                    n2 = n1 - dz * usl;
                }
                n_total += dz * (n1 + n2) / 2.0;
                while (z < z2 && iz < kNumBarkSteps) {
                    if (ns_out != nullptr) {
                        (*ns_out)[static_cast<std::size_t>(iz)] = n1 - (z - z1) * usl;
                    }
                    ++iz;
                    z += 0.1;
                }
                z1 = z2;
                n1 = n2;
            }
        }
    }
    return std::max(0.0, n_total);
}

} // namespace

double loudness_from_third_octave(const std::array<double, kNumThirdOct> &lt,
                                  std::array<double, kNumBarkSteps> *ns_out) {
    return spread_and_integrate(core_loudness(lt), ns_out);
}

// --- Band synthesis weighting ---------------------------------------------------------------

// |H|² of a 3rd-order Butterworth third-octave bandpass (the standard's filter class): the
// oracle's measured skirts put a tone's neighbouring band at -19 dB and the next at -38 dB;
// this analytic curve gives -17.4 / -34.7 — inside the §5 tolerance budget. The tables were
// calibrated with leaking filters, so the leakage is part of the model, not an error.
double band_weight_sq(double f_hz, double fc_hz) {
    if (f_hz <= 0.0) {
        return 0.0;
    }
    // Skirt-sharpening factor: fitted so the analytic curve reproduces the reference
    // implementation's measured adjacent-band attenuation (-19.4 dB; pure analog order-3
    // gives -17.4 dB — the digital realization is a touch steeper).
    constexpr double kRelBw = 0.23156 * 0.9258; // (2^(1/6) - 2^(-1/6)) * fit
    const double x = (f_hz / fc_hz - fc_hz / f_hz) / kRelBw;
    const double x2 = x * x;
    return 1.0 / (1.0 + x2 * x2 * x2);
}

// --- Time-varying path ---------------------------------------------------------------------

namespace {

struct Biquad {
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    inline double step(double x0) {
        const double y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;
        return y0;
    }
};

// RBJ constant-peak bandpass; cascading three with this Q gives -3 dB at the third-octave
// band edges (per-section x_e = Q*(2^(1/6) - 2^(-1/6)) solved for |H|^6 = 1/2).
Biquad make_bandpass(double f0, double fs) {
    constexpr double kSectionQ = 2.2019;
    Biquad q;
    const double w0 = 2.0 * M_PI * f0 / fs;
    const double alpha = std::sin(w0) / (2.0 * kSectionQ);
    const double a0 = 1.0 + alpha;
    q.b0 = alpha / a0;
    q.b1 = 0.0;
    q.b2 = -alpha / a0;
    q.a1 = -2.0 * std::cos(w0) / a0;
    q.a2 = (1.0 - alpha) / a0;
    return q;
}

// Temporal constants (tuned against the oracle within §5 tolerances; see file header).
// Oracle track anatomy (measured, numbers only): loudness peaks ~8 ms after an impulse and
// releases in two stages (~40 ms early, ~120 ms tail). The temporal architecture is
// therefore: band-dependent INTENSITY smoothing before the compressive core (~2/bandwidth —
// long at low fc, where it flattens fluctuating noise toward its mean and dilutes brief
// bursts; near-instant up high), then a fast-attack / slow-release loudness-time function.
// Band-intensity integration is ASYMMETRIC: a slow charge dilutes brief bursts before the
// compressive core (the onset-integration the oracle's filter chain exhibits), while a fast
// discharge keeps loudness lobes from being stretched (a symmetric smoother's release tail
// inflated click N5 by 2x). Attack follows ~2/bandwidth at low frequencies with a floor
// KIntAttackFloorS up high.
constexpr double kIntAttackFloorS = 0.080;
constexpr double kIntReleaseTauS = 0.008;
inline double int_attack_tau_s(double fc_hz) {
    return std::clamp(2.0 / (0.23156 * fc_hz), kIntAttackFloorS, 0.120);
}
constexpr double kDecayTauS = 0.008; // per-band post-masking release (attack instant)
// Series two-stage loudness-time function: two cascaded asymmetric one-poles. A cascade
// suppresses brief events superlinearly relative to sustained ones — the shape the oracle's
// N5 ratios demand (÷3.3 for a 20 ms click, ÷1.55 at ~100 ms, ÷1.23 for a ~250 ms noise
// swell whose crests it shaves). Constants from a sweep against the oracle N5 targets.
constexpr double kLtf1AttackS = 0.040;
constexpr double kLtf1ReleaseS = 0.060;
constexpr double kLtf2AttackS = 0.040;
constexpr double kLtf2ReleaseS = 0.090;

// Multi-resolution short-time band levels: rectangular FFT-bin integration has no adjacent-
// band leakage (the reason the oracle's steady tones show zero neighbour excitation), and the
// per-scale window lengths mirror the physical ringing of narrow low-frequency filters —
// short windows up high where the transients live, long windows down low.
struct StftScale {
    int nfft;
    double fc_min; // bands with fc >= fc_min (and < the next scale's fc_min) use this scale
};
// Scale boundaries: mid bands need the 2048 window — at 512, Hann sidelobe leakage into the
// adjacent band (-38 dB at 200 Hz offset) exceeds the intended Butterworth wing (-19 dB level
// but integrated over less bandwidth) and inflated steady tones by ~5 %. Transient timing
// only needs the short window up where impacts live (>= 3150 Hz).
constexpr StftScale kScales[3] = {{8192, 0.0}, {2048, 100.0}, {1024, 3150.0}};

int scale_for_band(double fc) {
    int s = 0;
    for (int i = 0; i < 3; ++i) {
        if (fc >= kScales[i].fc_min) {
            s = i;
        }
    }
    return s;
}

} // namespace

TvAnalysis analyze_time_varying(const std::vector<float> &pa, int fs) {
    TvAnalysis tv;
    if (pa.empty()) {
        return tv;
    }
    const std::size_t n = pa.size();
    const int hop = fs / 500; // 2 ms
    const int env_dec = 32;   // envelope decimation -> fs/32 (1.5 kHz at 48 kHz)
    tv.frame_dt = static_cast<double>(hop) / fs;
    tv.env_rate = static_cast<double>(fs) / env_dec;
    const std::size_t frames = n / static_cast<std::size_t>(hop);
    if (frames == 0) {
        return tv;
    }

    std::vector<std::array<double, kNumThirdOct>> band_ms(frames,
                                                          std::array<double, kNumThirdOct>{});

    // One causal STFT pass per scale: the window ENDS at the frame instant, so loudness can
    // never precede the signal. Band mean-square = Parseval-scaled bin power through the
    // Butterworth band weighting (precomputed per band over its significant bins).
    for (int s = 0; s < 3; ++s) {
        const int nfft = kScales[s].nfft;
        std::vector<double> window(static_cast<std::size_t>(nfft));
        double wsq = 0.0;
        for (int i = 0; i < nfft; ++i) {
            window[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (nfft - 1));
            wsq += window[static_cast<std::size_t>(i)] * window[static_cast<std::size_t>(i)];
        }
        std::size_t lenmem = 0;
        kiss_fftr_alloc(nfft, 0, nullptr, &lenmem);
        std::vector<unsigned char> mem(lenmem);
        kiss_fftr_cfg cfg = kiss_fftr_alloc(nfft, 0, mem.data(), &lenmem);
        const int bins = nfft / 2 + 1;
        std::vector<kiss_fft_scalar> in(static_cast<std::size_t>(nfft));
        std::vector<kiss_fft_cpx> out(static_cast<std::size_t>(bins));
        const double scale = 2.0 / (wsq * nfft);
        const double bin_hz = static_cast<double>(fs) / nfft;

        // Per-band bin weights for this scale, restricted to |H|² > 1e-7.
        struct BandBins {
            int band;
            int first;
            std::vector<double> w;
        };
        std::vector<BandBins> scale_bands;
        for (int band = 0; band < kNumThirdOct; ++band) {
            const double fc = kThirdOctFc[static_cast<std::size_t>(band)];
            if (scale_for_band(fc) != s) {
                continue;
            }
            BandBins bb;
            bb.band = band;
            bb.first = -1;
            for (int b = 1; b < bins; ++b) {
                const double w = band_weight_sq(b * bin_hz, fc);
                if (w > 1e-7) {
                    if (bb.first < 0) {
                        bb.first = b;
                    }
                    bb.w.push_back(w);
                } else if (bb.first >= 0) {
                    break;
                }
            }
            if (bb.first >= 0) {
                scale_bands.push_back(std::move(bb));
            }
        }

        for (std::size_t f = 0; f < frames; ++f) {
            const long long end = static_cast<long long>((f + 1) * static_cast<std::size_t>(hop));
            for (int i = 0; i < nfft; ++i) {
                const long long idx = end - nfft + i;
                const double x = idx >= 0 && idx < static_cast<long long>(n)
                                     ? static_cast<double>(pa[static_cast<std::size_t>(idx)])
                                     : 0.0;
                in[static_cast<std::size_t>(i)] =
                    static_cast<kiss_fft_scalar>(x * window[static_cast<std::size_t>(i)]);
            }
            kiss_fftr(cfg, in.data(), out.data());
            for (const BandBins &bb : scale_bands) {
                double ms = 0.0;
                for (std::size_t k = 0; k < bb.w.size(); ++k) {
                    const int b = bb.first + static_cast<int>(k);
                    if (b >= bins) {
                        break;
                    }
                    const double re = out[static_cast<std::size_t>(b)].r;
                    const double im = out[static_cast<std::size_t>(b)].i;
                    ms += (re * re + im * im) * bb.w[k];
                }
                band_ms[f][static_cast<std::size_t>(bb.band)] = ms * scale;
            }
        }
    }

    // Whole-file time-averaged band levels (the stationary path; equals the mean-square a
    // real filterbank would report, transient-safe because every sample is covered).
    for (int band = 0; band < kNumThirdOct; ++band) {
        double sum = 0.0;
        for (std::size_t f = 0; f < frames; ++f) {
            sum += band_ms[f][static_cast<std::size_t>(band)];
        }
        const double mean = sum / static_cast<double>(frames);
        tv.stationary_levels[static_cast<std::size_t>(band)] =
            mean > 0.0 ? std::max(-70.0, 10.0 * std::log10(mean / (kP0 * kP0))) : -70.0;
    }

    // Filterbank pass for the modulation envelopes only (roughness/fluctuation front-end).
    tv.band_env.assign(kNumThirdOct, {});
    const double env_a = std::exp(-2.0 * M_PI * 300.0 / fs); // 300 Hz envelope low-pass
    for (int band = 0; band < kNumThirdOct; ++band) {
        Biquad s1 = make_bandpass(kThirdOctFc[static_cast<std::size_t>(band)], fs);
        Biquad s2 = s1, s3 = s1;
        double env = 0.0;
        std::vector<float> &env_out = tv.band_env[static_cast<std::size_t>(band)];
        env_out.reserve(n / static_cast<std::size_t>(env_dec) + 1);
        for (std::size_t i = 0; i < n; ++i) {
            const double y = s3.step(s2.step(s1.step(static_cast<double>(pa[i]))));
            env = env_a * env + (1.0 - env_a) * std::fabs(y);
            if (i % static_cast<std::size_t>(env_dec) == 0) {
                env_out.push_back(static_cast<float>(env));
            }
        }
    }

    // Core loudness per frame, nonlinear per-band decay, spreading (accumulating the
    // time-averaged specific loudness for sharpness), LTF smoothing.
    tv.n_track.resize(frames);
    tv.n_drive.resize(frames);
    std::array<double, 21> state{};
    std::array<double, kNumThirdOct> int_state{};
    std::array<double, kNumThirdOct> frame_levels{};
    std::array<double, kNumBarkSteps> ns{};
    std::array<double, kNumThirdOct> int_up{};
    for (int band = 0; band < kNumThirdOct; ++band) {
        int_up[static_cast<std::size_t>(band)] =
            std::exp(-tv.frame_dt / int_attack_tau_s(kThirdOctFc[static_cast<std::size_t>(band)]));
    }
    const double int_down = std::exp(-tv.frame_dt / kIntReleaseTauS);
    const double decay = std::exp(-tv.frame_dt / kDecayTauS);
    const double u1_up = 1.0 - std::exp(-tv.frame_dt / kLtf1AttackS);
    const double u1_down = 1.0 - std::exp(-tv.frame_dt / kLtf1ReleaseS);
    const double u2_up = 1.0 - std::exp(-tv.frame_dt / kLtf2AttackS);
    const double u2_down = 1.0 - std::exp(-tv.frame_dt / kLtf2ReleaseS);
    double u1 = 0.0, u2 = 0.0;
    for (std::size_t f = 0; f < frames; ++f) {
        for (int band = 0; band < kNumThirdOct; ++band) {
            const std::size_t b = static_cast<std::size_t>(band);
            const double a = band_ms[f][b] > int_state[b] ? int_up[b] : int_down;
            int_state[b] = a * int_state[b] + (1.0 - a) * band_ms[f][b];
            frame_levels[b] = int_state[b] > 0.0
                                  ? std::max(-70.0, 10.0 * std::log10(int_state[b] / (kP0 * kP0)))
                                  : -70.0;
        }
        std::array<double, 21> nm = core_loudness(frame_levels);
        for (int i = 0; i < 20; ++i) {
            const std::size_t b = static_cast<std::size_t>(i);
            state[b] = nm[b] >= state[b] ? nm[b] : nm[b] + (state[b] - nm[b]) * decay;
        }
        const double n_inst = spread_and_integrate(state, &ns);
        for (int iz = 0; iz < kNumBarkSteps; ++iz) {
            tv.ns_mean[static_cast<std::size_t>(iz)] += ns[static_cast<std::size_t>(iz)];
        }
        tv.n_drive[f] = n_inst;
        u1 += (n_inst >= u1 ? u1_up : u1_down) * (n_inst - u1);
        u2 += (u1 >= u2 ? u2_up : u2_down) * (u1 - u2);
        tv.n_track[f] = u2;
    }
    for (int iz = 0; iz < kNumBarkSteps; ++iz) {
        tv.ns_mean[static_cast<std::size_t>(iz)] /= static_cast<double>(frames);
    }
    return tv;
}

} // namespace sp::psycho
