#pragma once

#include "soundpalette/audio.h"
#include "soundpalette/mapping.h"

namespace sp {

// Psychoacoustic metrics (extension-3 §5), computed on the ORIGINAL-GAIN buffer under the §4
// calibration convention: a -23 LUFS signal is assumed to play at ref_spl dB SPL, so
// p[n] = K * x[n] with K = 20 µPa * 10^((ref_spl + 23) / 20).
//
// Implementations follow the published algorithms (ISO 532-1 Zwicker loudness, DIN 45692
// sharpness, Daniel & Weber roughness, Zwicker & Fastl fluctuation strength) with the §5
// tolerance table as the arbiter; simplifications are documented at the implementation site
// and in NOTES.md. No oracle (MoSQITo) code is ported — only its numbers gate ours.
struct PsychoFeatures {
    double ref_spl = 0.0;                 // convention used (§4)
    double sones_n5 = 0.0;                // ISO 532-1 time-varying loudness, N5 percentile
    double sones_mean = 0.0;              // mean of the loudness-vs-time track
    double sharpness_acum = 0.0;          // DIN 45692, from time-averaged specific loudness
    double roughness_asper = 0.0;         // Daniel & Weber
    double fluctuation_vacil = 0.0;       // shared modulation front-end at ~4 Hz
    bool experimental_fluctuation = true; // always true in mapping v2 (§5)
};

PsychoFeatures compute_psycho(const AudioBuffer &original_gain, const MappingConfig &config);

} // namespace sp
