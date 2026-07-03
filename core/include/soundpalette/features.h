#pragma once

#include <array>

#include "soundpalette/audio.h"

namespace sp {

// Perceptual audio features, extracted on the −23 LUFS-normalized copy of an AudioBuffer (§6).
struct Features {
    double centroid_hz = 0.0;
    double rolloff85_hz = 0.0;
    double flatness = 0.0;
    double zcr = 0.0;
    double attack_s = 0.0;
    double tail_s = 0.0;
    double roughness = 0.0;
    double warmth = 0.0;
    std::array<double, 6> bands{}; // sub, low, lowmid, mid, high, air (§6)
    bool tail_clipped = false;
};

// STFT N=2048, hop=512, Hann window (§6). loudness must have been measured on `buffer` first.
// Silent buffers (loudness.silent) return default-constructed (all-zero) Features.
Features extract_features(const AudioBuffer& buffer, const Loudness& loudness);

} // namespace sp
