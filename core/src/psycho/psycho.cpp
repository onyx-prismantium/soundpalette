// compute_psycho: orchestrates the psychoacoustic block (extension-3 §5) on the ORIGINAL-
// GAIN buffer under the §4 calibration convention. Deterministic: fixed algorithms, no
// threading, doubles throughout.

#include "soundpalette/psycho.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "psycho_internal.h"

namespace sp {

namespace {

double percentile95(std::vector<double> track) {
    if (track.empty()) {
        return 0.0;
    }
    std::sort(track.begin(), track.end());
    // Linear interpolation between closest ranks (matches the oracle's percentile method).
    const double pos = 0.95 * static_cast<double>(track.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(pos);
    const std::size_t hi = std::min(lo + 1, track.size() - 1);
    const double frac = pos - static_cast<double>(lo);
    return track[lo] + (track[hi] - track[lo]) * frac;
}

} // namespace

PsychoFeatures compute_psycho(const AudioBuffer &original_gain, const MappingConfig &config) {
    PsychoFeatures out;
    out.ref_spl = config.ref_spl;
    out.experimental_fluctuation = true;
    const std::vector<float> &x = original_gain.samples48k_mono;
    if (x.empty()) {
        return out;
    }

    // §4: pressure in pascals; a -23 LUFS signal plays at ref_spl dB SPL.
    const double k = psycho::kP0 * std::pow(10.0, (config.ref_spl + 23.0) / 20.0);
    std::vector<float> pa(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        pa[i] = static_cast<float>(static_cast<double>(x[i]) * k);
    }

    // One analysis pass: time-varying loudness -> N5/mean; time-averaged specific loudness
    // -> DIN 45692 sharpness; band envelopes -> roughness/fluctuation.
    psycho::TvAnalysis tv = psycho::analyze_time_varying(pa, 48000);
    double ns_total = 0.0;
    for (double v : tv.ns_mean) {
        ns_total += v * 0.1;
    }
    out.sharpness_acum = psycho::sharpness_din_from_ns(tv.ns_mean, ns_total);
    if (!tv.n_track.empty()) {
        out.sones_n5 = percentile95(tv.n_track);
        double sum = 0.0;
        for (double n : tv.n_track) {
            sum += n;
        }
        out.sones_mean = sum / static_cast<double>(tv.n_track.size());
    }
    psycho::ModulationMetrics mod = psycho::modulation_metrics(tv);
    out.roughness_asper = mod.roughness_asper;
    out.fluctuation_vacil = mod.fluctuation_vacil;
    return out;
}

} // namespace sp
