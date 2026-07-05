#pragma once

// Internal seams of the psychoacoustic engine (extension-3 §5). Everything here operates on
// CALIBRATED sound pressure in pascals (the §4 K factor is applied by compute_psycho).

#include <array>
#include <vector>

namespace sp::psycho {

inline constexpr int kNumThirdOct = 28;   // 25 Hz .. 12.5 kHz
inline constexpr int kNumBarkSteps = 240; // specific loudness N'(z), 0.1 bark steps to 24
inline constexpr double kP0 = 2e-5;       // 20 µPa reference pressure

// Third-octave centre frequencies, 25 Hz .. 12.5 kHz.
extern const std::array<double, kNumThirdOct> kThirdOctFc;

// Stationary Zwicker loudness (ISO 532-1 / DIN 45631 tables) from 28 third-octave band
// levels in dB SPL, free field. Returns total loudness in sones; when ns_out is non-null it
// receives the 240-point specific-loudness pattern N'(z) in sone/bark.
double loudness_from_third_octave(const std::array<double, kNumThirdOct> &lt,
                                  std::array<double, kNumBarkSteps> *ns_out);

// Third-octave band magnitude-squared response (analytic 3rd-order Butterworth bandpass,
// matching the standard's filter class): ISO 532-1's equal-loudness tables were calibrated
// WITH these skirts, so band synthesis must leak like the filters do (a pure tone excites
// its neighbours ~-17..-19 dB down) — rectangular bin integration undershoots loudness by
// 8-15 % on tonal content.
double band_weight_sq(double f_hz, double fc_hz);

// Time-varying analysis: multi-resolution causal STFT -> Butterworth-weighted band
// intensities at 2 ms steps -> per-frame core loudness -> nonlinear temporal decay per
// critical band -> spreading -> loudness-time function. Also derives the whole-file
// time-averaged band levels + specific loudness (the stationary/sharpness path) and per-band
// envelopes (filterbank) for the modulation front-end.
struct TvAnalysis {
    std::vector<double> n_track; // total loudness per 2 ms frame (sones)
    std::vector<double> n_drive; // pre-LTF instantaneous loudness (diagnostics/tuning)
    double frame_dt = 0.002;
    std::array<double, kNumThirdOct> stationary_levels{}; // 10·log10(mean band ms / p0²)
    std::array<double, kNumBarkSteps> ns_mean{};          // time-averaged N'(z), post-decay
    std::vector<std::vector<float>> band_env; // [band][t], |bandpass| LP-smoothed, fs/32
    double env_rate = 1500.0;
};
TvAnalysis analyze_time_varying(const std::vector<float> &pa, int fs);

// DIN 45692 sharpness from a specific-loudness pattern.
double sharpness_din_from_ns(const std::array<double, kNumBarkSteps> &ns, double n_total);

// Daniel & Weber-style roughness and the shared-front-end fluctuation strength from the
// filterbank band envelopes.
struct ModulationMetrics {
    double roughness_asper = 0.0;
    double fluctuation_vacil = 0.0;
};
ModulationMetrics modulation_metrics(const TvAnalysis &tv);

} // namespace sp::psycho
