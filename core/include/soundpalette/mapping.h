#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "soundpalette/features.h"

namespace sp {

struct PsychoFeatures; // psycho.h; by-reference here to avoid a header cycle

// Visual attributes the analysis maps to (§7 + extension-3 §6). HSL color space.
struct Visual {
    double hue_deg = 0.0;
    double sat = 0.0;
    double light = 0.0;
    double size_px = 0.0;
    double spike01 = 0.0;
    double jitter01 = 0.0;
    double tail01 = 0.0;
    double fluct01 = 0.0; // mapping v2: slow three-lobe wave, distinct from jitter's grain
    int spikes = 0;
    std::uint64_t seed = 0;
};

// Every mapping v1 constant (§7), grouped as in the plan's table. Defaults are the v1 spec;
// the GUI tuner (§10) edits a runtime copy via set_active_mapping_config(). This struct is the
// project's one sanctioned piece of global mutable state (§15): the app's active runtime config.
struct MappingConfig {
    int mapping_version = 2;

    // bright01 = lin01(log2(centroid_hz), log2(bright_lo_hz), log2(bright_hi_hz))
    double bright_lo_hz = 200.0;
    double bright_hi_hz = 8000.0;

    // warm01 = lin01(warmth, warm_lo, warm_hi)
    double warm_lo = 0.10;
    double warm_hi = 0.70;

    // ton01 = 1 - lin01(flatness, 0, ton_flatness_hi)
    double ton_flatness_hi = 0.50;

    // loud01 = lin01(lufs_i, loud_lo_lufs, loud_hi_lufs)
    double loud_lo_lufs = -40.0;
    double loud_hi_lufs = -10.0;

    // atk01 = 1 - log01(attack_s, atk_lo_s, atk_hi_s)
    double atk_lo_s = 0.002;
    double atk_hi_s = 0.150;

    // tail01 = log01(tail_s, tail_lo_s, tail_hi_s)
    double tail_lo_s = 0.05;
    double tail_hi_s = 3.0;

    // flat01 = lin01(flatness, 0, flat_hi)
    double flat_hi = 0.60;

    // rough01 = clamp(roughness * rough_scale, 0, 1) -- TUNABLE (§7)
    double rough_scale = 4.0;

    // jitter01 = jitter_flat_weight*flat01 + jitter_rough_weight*rough01
    double jitter_flat_weight = 0.7;
    double jitter_rough_weight = 0.3;

    // hue_deg = hue_base_deg - hue_warm_span_deg*warm01
    double hue_base_deg = 220.0;
    double hue_warm_span_deg = 200.0;

    // sat = sat_base + sat_ton_span*ton01 (percent)
    double sat_base = 25.0;
    double sat_ton_span = 60.0;

    // light = light_base + light_bright_span*bright01 (percent)
    double light_base = 28.0;
    double light_bright_span = 50.0;

    // size_px = size_base_px + size_loud_span_px*loud01
    double size_base_px = 14.0;
    double size_loud_span_px = 50.0;

    // spikes = (atk01 > spike_threshold) ? round(spike_count_base + spike_count_span*atk01) : 0
    double spike_threshold = 0.35;
    double spike_count_base = 4.0;
    double spike_count_span = 10.0;

    // Mapping v2 perceptual dims (extension-3 §6), all TUNABLE:
    //   loud01   = clamp(sqrt(sones_n5) / loud_sone_div, 0, 1)
    //   bright01 = lin01(sharpness_acum, bright_acum_lo, bright_acum_hi)
    //   jitter01 = lin01(roughness_asper, jitter_asper_lo, jitter_asper_hi)
    //   fluct01  = lin01(fluctuation_vacil, fluct_vacil_lo, fluct_vacil_hi)
    double loud_sone_div = 6.0;
    double bright_acum_lo = 0.6;
    double bright_acum_hi = 3.5;
    double jitter_asper_lo = 0.02;
    double jitter_asper_hi = 0.60;
    double fluct_vacil_lo = 0.02;
    double fluct_vacil_hi = 1.0;

    // Glyph area proportional to loudness (sones are a ratio scale):
    // size_px = clamp(size_sone_base + size_sone_scale * sqrt(sones_n5), 12, 72).
    double size_sone_base_px = 10.0;
    double size_sone_scale_px = 11.0;

    // Fluctuation wave amplitude in glyph_outline: r += fluct01 * fluct_wave_amp * size_px.
    double fluct_wave_amp = 0.18;

    // JND phrasing constants (§7, Zwicker & Fastl-order approximations, TUNABLE):
    // loudness JND count = ln(ratio) / ln(jnd_loud_ratio); other dims count =
    // |delta| / (jnd_fraction * family median), median floored at the dim's lower bound.
    double jnd_loud_ratio = 1.10;
    double jnd_fraction = 0.10;

    // Psychoacoustic calibration (extension-3 §4): a -23 LUFS signal is assumed to play at
    // ref_spl dB SPL. Allowed range 60-85; changing it invalidates every psycho block, so it
    // is stamped into manifests/profiles and mismatches are refused, never remapped.
    double ref_spl = 75.0;

    // Silent-file fixed visual (§7).
    double silent_hue_deg = 0.0;
    double silent_sat = 0.0;
    double silent_light = 60.0;
    double silent_size_px = 8.0;
};

// The project's one sanctioned global mutable state (§15): the app's active runtime mapping
// config. Defaults to the v1 spec; the GUI tuner mutates it, print-mapping and scan read it.
const MappingConfig &active_mapping_config();
void set_active_mapping_config(const MappingConfig &config);
void reset_active_mapping_config(); // back to v1 defaults

// FNV-1a 64-bit hash of a manifest-relative path, used to seed glyph jitter deterministically.
std::uint64_t path_seed(std::string_view relative_path);

// Maps the analysis to a Visual using the currently active MappingConfig (§7 rules on the v2
// dims). Silent loudness short-circuits to the fixed silent visual.
Visual map_v2(const Features &features, const Loudness &loudness, const PsychoFeatures &psycho,
              std::uint64_t seed);

// The eight normalized dimensions lint (§9) and manifest stats (§8) operate over, in a fixed
// order: [bright01, warm01, ton01, atk01, tail01, loud01, jitter01, fluct01]. bright01,
// loud01, jitter01, and fluct01 come from the psychoacoustic block (extension-3 §6).
std::array<double, 8> mapping_dims(const Features &features, const Loudness &loudness,
                                   const PsychoFeatures &psycho);

// Canonical JSON dump of a MappingConfig (used by `print-mapping` and the GUI tuner's
// "Export mapping.json"; matches assets/mapping_v2.json for the v1 defaults).
std::string mapping_config_to_json(const MappingConfig &config);

} // namespace sp
