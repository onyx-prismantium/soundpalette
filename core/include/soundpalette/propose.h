#pragma once

#include <array>

#include "soundpalette/manifest.h"
#include "soundpalette/recipe.h"

namespace sp {

// Solver heuristics (extension §6.4). All constants TUNABLE; the log-space damping nudges for
// attack/tail follow from atk01/tail01 being linear in log10 of their underlying seconds.
struct SolverConfig {
    double pull_margin_sigma = 0.5;  // v* = mean + sign(z) * (threshold - margin) * std
    double stop_margin = 0.25;       // stop when max|z| <= threshold - stop_margin
    double damping = 0.7;            // proportional correction factor per iteration
    double bright_shelf_hz = 4000.0; // bright01 -> high_shelf frequency
    double bright_slope = 0.011;     // dim units per dB of high-shelf gain
    double warm_shelf_hz = 250.0;    // warm01 -> low_shelf frequency
    double warm_slope = 0.007;       // dim units per dB of low-shelf gain
    double shelf_clamp_db = 12.0;    // +-12 dB shelf clamp
    double atk_fade_scale = 0.59;    // fade_ms = (attack_s* / scale) * 1000
    double fade_clamp_lo_ms = 5.0;
    double fade_clamp_hi_ms = 120.0;
};

// Deterministic recipe proposer (extension §6.4): pure function of its inputs — no RNG, no
// wall clock. Iterates apply-in-memory -> re-analyze -> correct magnitudes (damped), at most
// max_iter times. Offenses without a tier-one inverse go to result_unresolved.
Recipe propose_recipe(const NativeAudio &audio, const Features &features, const Loudness &loudness,
                      const std::array<DimStats, 7> &baseline_stats, double threshold,
                      int max_iter = 3, const SolverConfig &config = SolverConfig{});

} // namespace sp
