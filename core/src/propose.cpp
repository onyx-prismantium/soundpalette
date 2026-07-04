// Deterministic tier-one recipe proposer (extension §6.4).

#include <algorithm>
#include <cmath>

#include "soundpalette/mapping.h"
#include "soundpalette/propose.h"

namespace sp {

namespace {

constexpr const char *kDimNames[7] = {"bright01", "warm01", "ton01",   "atk01",
                                      "tail01",   "loud01", "jitter01"};
constexpr int kBright = 0, kWarm = 1, kAtk = 3, kTail = 4, kLoud = 5;

std::array<double, 7> z_scores(const std::array<double, 7> &dims,
                               const std::array<DimStats, 7> &stats) {
    std::array<double, 7> z{};
    for (std::size_t d = 0; d < 7; ++d) {
        z[d] = (dims[d] - stats[d].mean) / std::max(stats[d].std, 0.02); // §9 floor
    }
    return z;
}

double max_abs(const std::array<double, 7> &z) {
    double m = 0.0;
    for (double v : z) {
        m = std::max(m, std::fabs(v));
    }
    return m;
}

// Inverts §7's atk01 = 1 - log01(attack_s, 0.002, 0.150).
double invert_atk01(double atk01) {
    const double lo = std::log10(0.002), hi = std::log10(0.150);
    return std::pow(10.0, lo + (1.0 - std::clamp(atk01, 0.0, 1.0)) * (hi - lo));
}

// Inverts §7's tail01 = log01(tail_s, 0.05, 3.0).
double invert_tail01(double tail01) {
    const double lo = std::log10(0.05), hi = std::log10(3.0);
    return std::pow(10.0, lo + std::clamp(tail01, 0.0, 1.0) * (hi - lo));
}

} // namespace

Recipe propose_recipe(const NativeAudio &audio, const Features &features, const Loudness &loudness,
                      const std::array<DimStats, 7> &baseline_stats, double threshold, int max_iter,
                      const SolverConfig &cfg) {
    Recipe recipe;
    recipe.target_threshold = threshold;

    const std::array<double, 7> dims0 = mapping_dims(features, loudness);
    const std::array<double, 7> z0 = z_scores(dims0, baseline_stats);
    recipe.result_max_z_before = max_abs(z0);

    // Step 1-3 (§6.4): map each offense to a tier-one op or to unresolved[].
    std::array<double, 7> target{}; // pulled inside with the half-sigma margin
    std::array<bool, 7> offending{};
    for (std::size_t d = 0; d < 7; ++d) {
        offending[d] = std::fabs(z0[d]) >= threshold;
        if (offending[d]) {
            const double sign = z0[d] > 0 ? 1.0 : -1.0;
            target[d] = baseline_stats[d].mean + sign * (threshold - cfg.pull_margin_sigma) *
                                                     std::max(baseline_stats[d].std, 0.02);
            recipe.result_dims_before[kDimNames[d]] = dims0[d];
        }
    }

    Op high_shelf, low_shelf, attack, tail, gain;
    bool use_hs = false, use_ls = false, use_atk = false, use_tail = false, use_gain = false;
    std::array<bool, 7> resolvable{};

    if (offending[kBright]) {
        use_hs = true;
        resolvable[kBright] = true;
        high_shelf.op = OpType::kHighShelf;
        high_shelf.freq_hz = cfg.bright_shelf_hz;
        high_shelf.gain_db = std::clamp((target[kBright] - dims0[kBright]) / cfg.bright_slope,
                                        -cfg.shelf_clamp_db, cfg.shelf_clamp_db);
    }
    if (offending[kWarm]) {
        use_ls = true;
        resolvable[kWarm] = true;
        low_shelf.op = OpType::kLowShelf;
        low_shelf.freq_hz = cfg.warm_shelf_hz;
        low_shelf.gain_db = std::clamp((target[kWarm] - dims0[kWarm]) / cfg.warm_slope,
                                       -cfg.shelf_clamp_db, cfg.shelf_clamp_db);
    }
    if (offending[kAtk]) {
        if (z0[kAtk] > 0) { // too high only (§6.4): soften; there is no tier-one sharpener
            use_atk = true;
            resolvable[kAtk] = true;
            attack.op = OpType::kAttackSoften;
            attack.fade_ms = std::clamp(invert_atk01(target[kAtk]) / cfg.atk_fade_scale * 1000.0,
                                        cfg.fade_clamp_lo_ms, cfg.fade_clamp_hi_ms);
        } else {
            recipe.result_unresolved.push_back("atk01");
        }
    }
    if (offending[kTail]) {
        if (z0[kTail] > 0) { // too high only: shorten; lengthening is v3 (§10)
            use_tail = true;
            resolvable[kTail] = true;
            tail.op = OpType::kTailShorten;
            tail.target_tail_s = invert_tail01(target[kTail]);
        } else {
            recipe.result_unresolved.push_back("tail01");
        }
    }
    if (offending[kLoud]) {
        use_gain = true;
        resolvable[kLoud] = true;
        gain.op = OpType::kGainToLufs;
        gain.target_lufs = -40.0 + 30.0 * baseline_stats[kLoud].mean; // §6.4 lufs*
        gain.tp_ceiling_db = -1.0;
    }
    for (std::size_t d = 0; d < 7; ++d) {
        if (offending[d] && !resolvable[d] && kDimNames[d] != std::string("atk01") &&
            kDimNames[d] != std::string("tail01")) {
            recipe.result_unresolved.push_back(kDimNames[d]);
        }
    }

    // Step 4: iterate apply -> re-analyze -> damped correction (§6.4).
    std::array<double, 7> dims_k = dims0;
    ApplyReport last_report;
    int iterations = 0;
    bool converged = false;

    for (int k = 1; k <= max_iter; ++k) {
        iterations = k;
        std::vector<Op> ops; // normative auto order: shelves -> attack -> tail -> gain last
        if (use_hs) {
            ops.push_back(high_shelf);
        }
        if (use_ls) {
            ops.push_back(low_shelf);
        }
        if (use_atk) {
            ops.push_back(attack);
        }
        if (use_tail) {
            ops.push_back(tail);
        }
        if (use_gain) {
            ops.push_back(gain);
        }
        recipe.ops = ops;
        if (ops.empty()) {
            break; // nothing tier-one can do; report stands on the unresolved list
        }

        NativeAudio work = audio; // fresh copy each iteration: ops are absolute, not stacked
        last_report = ApplyReport{};
        apply_chain(work, ops, last_report);

        Loudness post_loudness;
        Features post_features;
        analyze_native(work, post_loudness, post_features);
        dims_k = mapping_dims(post_features, post_loudness);
        const std::array<double, 7> z_k = z_scores(dims_k, baseline_stats);

        double worst_resolvable = 0.0;
        for (std::size_t d = 0; d < 7; ++d) {
            if (resolvable[d]) {
                worst_resolvable = std::max(worst_resolvable, std::fabs(z_k[d]));
            }
        }
        if (worst_resolvable <= threshold - cfg.stop_margin) {
            converged = true;
            break;
        }

        // Damped proportional corrections toward the pulled-in targets.
        if (use_hs) {
            high_shelf.gain_db =
                std::clamp(high_shelf.gain_db +
                               cfg.damping * (target[kBright] - dims_k[kBright]) / cfg.bright_slope,
                           -cfg.shelf_clamp_db, cfg.shelf_clamp_db);
        }
        if (use_ls) {
            low_shelf.gain_db = std::clamp(
                low_shelf.gain_db + cfg.damping * (target[kWarm] - dims_k[kWarm]) / cfg.warm_slope,
                -cfg.shelf_clamp_db, cfg.shelf_clamp_db);
        }
        if (use_atk) {
            // atk01 is linear in log10(attack_s): nudge the fade in log space.
            const double span = std::log10(0.150) - std::log10(0.002);
            const double delta = cfg.damping * (dims_k[kAtk] - target[kAtk]) * span;
            attack.fade_ms = std::clamp(attack.fade_ms * std::pow(10.0, delta),
                                        cfg.fade_clamp_lo_ms, cfg.fade_clamp_hi_ms);
        }
        if (use_tail) {
            const double span = std::log10(3.0) - std::log10(0.05);
            const double delta = cfg.damping * (target[kTail] - dims_k[kTail]) * span;
            tail.target_tail_s = std::max(0.01, tail.target_tail_s * std::pow(10.0, delta));
        }
        // gain_to_lufs targets an absolute LUFS; no correction needed.
    }

    const std::array<double, 7> z_final = z_scores(dims_k, baseline_stats);
    recipe.result_iterations = iterations;
    recipe.result_converged = converged;
    recipe.result_limited_by_peak = last_report.limited_by_peak;
    recipe.result_max_z_after = max_abs(z_final);
    for (std::size_t d = 0; d < 7; ++d) {
        if (offending[d]) {
            recipe.result_dims_after[kDimNames[d]] = dims_k[d];
        }
    }

    // A tier-one op can fix a "no-inverse" dim incidentally when it shares a spectral cause
    // (e.g. one high-shelf moving bright01, ton01, and jitter01 together). unresolved[] keeps
    // only dims still offending after the final measurement — nothing silently dropped: a dim
    // is either inside the threshold now or listed here.
    std::vector<std::string> still_unresolved;
    for (const std::string &name : recipe.result_unresolved) {
        for (std::size_t d = 0; d < 7; ++d) {
            if (name == kDimNames[d] && std::fabs(z_final[d]) >= threshold) {
                still_unresolved.push_back(name);
            }
        }
    }
    recipe.result_unresolved = std::move(still_unresolved);
    std::sort(recipe.result_unresolved.begin(), recipe.result_unresolved.end());
    return recipe;
}

} // namespace sp
