#include "soundpalette/mapping.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include <nlohmann/json.hpp>

namespace sp {

namespace {

double lin01(double x, double lo, double hi) {
    return std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
}

double log01(double x, double lo, double hi) {
    return lin01(std::log10(x), std::log10(lo), std::log10(hi));
}

MappingConfig &mutable_active_config() {
    static MappingConfig config;
    return config;
}

std::mutex &active_config_mutex() {
    static std::mutex m;
    return m;
}

} // namespace

const MappingConfig &active_mapping_config() {
    std::lock_guard<std::mutex> lock(active_config_mutex());
    return mutable_active_config();
}

void set_active_mapping_config(const MappingConfig &config) {
    std::lock_guard<std::mutex> lock(active_config_mutex());
    mutable_active_config() = config;
}

void reset_active_mapping_config() {
    std::lock_guard<std::mutex> lock(active_config_mutex());
    mutable_active_config() = MappingConfig{};
}

std::uint64_t path_seed(std::string_view relative_path) {
    // FNV-1a 64 (§5).
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kPrime = 0x100000001b3ULL;
    for (unsigned char c : relative_path) {
        hash ^= c;
        hash *= kPrime;
    }
    return hash;
}

std::array<double, 7> mapping_dims(const Features &f, const Loudness &loudness) {
    // Copy the active config once so config changes are not observed mid-computation.
    MappingConfig c;
    {
        std::lock_guard<std::mutex> lock(active_config_mutex());
        c = mutable_active_config();
    }

    const double bright01 = log01(std::max(f.centroid_hz, 1e-9), c.bright_lo_hz, c.bright_hi_hz);
    const double warm01 = lin01(f.warmth, c.warm_lo, c.warm_hi);
    const double ton01 = 1.0 - lin01(f.flatness, 0.0, c.ton_flatness_hi);
    const double loud01 = lin01(loudness.lufs_i, c.loud_lo_lufs, c.loud_hi_lufs);
    const double atk01 = 1.0 - log01(std::max(f.attack_s, 1e-9), c.atk_lo_s, c.atk_hi_s);
    const double tail01 = log01(std::max(f.tail_s, 1e-9), c.tail_lo_s, c.tail_hi_s);
    const double flat01 = lin01(f.flatness, 0.0, c.flat_hi);
    const double rough01 = std::clamp(f.roughness * c.rough_scale, 0.0, 1.0);
    const double jitter01 = c.jitter_flat_weight * flat01 + c.jitter_rough_weight * rough01;

    return {bright01, warm01, ton01, atk01, tail01, loud01, jitter01};
}

Visual map_v1(const Features &features, const Loudness &loudness, std::uint64_t seed) {
    MappingConfig c;
    {
        std::lock_guard<std::mutex> lock(active_config_mutex());
        c = mutable_active_config();
    }

    Visual v;
    v.seed = seed;

    if (loudness.silent) {
        v.hue_deg = c.silent_hue_deg;
        v.sat = c.silent_sat;
        v.light = c.silent_light;
        v.size_px = c.silent_size_px;
        v.spike01 = 0.0;
        v.spikes = 0;
        v.jitter01 = 0.0;
        v.tail01 = 0.0;
        return v;
    }

    std::array<double, 7> dims = mapping_dims(features, loudness);
    const double bright01 = dims[0];
    const double warm01 = dims[1];
    const double ton01 = dims[2];
    const double atk01 = dims[3];
    const double tail01 = dims[4];
    const double loud01 = dims[5];
    const double jitter01 = dims[6];

    v.hue_deg = c.hue_base_deg - c.hue_warm_span_deg * warm01;
    v.sat = c.sat_base + c.sat_ton_span * ton01;
    v.light = c.light_base + c.light_bright_span * bright01;
    v.size_px = c.size_base_px + c.size_loud_span_px * loud01;
    v.spike01 = atk01;
    v.spikes = (atk01 > c.spike_threshold)
                   ? static_cast<int>(std::lround(c.spike_count_base + c.spike_count_span * atk01))
                   : 0;
    v.jitter01 = jitter01;
    v.tail01 = tail01;

    return v;
}

std::string mapping_config_to_json(const MappingConfig &c) {
    using json = nlohmann::ordered_json;
    json root = json::object();
    root["mapping_version"] = c.mapping_version;

    json bright = json::object();
    bright["lo_hz"] = c.bright_lo_hz;
    bright["hi_hz"] = c.bright_hi_hz;
    root["bright01"] = std::move(bright);

    json warm = json::object();
    warm["lo"] = c.warm_lo;
    warm["hi"] = c.warm_hi;
    root["warm01"] = std::move(warm);

    json ton = json::object();
    ton["flatness_hi"] = c.ton_flatness_hi;
    root["ton01"] = std::move(ton);

    json loud = json::object();
    loud["lo_lufs"] = c.loud_lo_lufs;
    loud["hi_lufs"] = c.loud_hi_lufs;
    root["loud01"] = std::move(loud);

    json atk = json::object();
    atk["lo_s"] = c.atk_lo_s;
    atk["hi_s"] = c.atk_hi_s;
    root["atk01"] = std::move(atk);

    json tail = json::object();
    tail["lo_s"] = c.tail_lo_s;
    tail["hi_s"] = c.tail_hi_s;
    root["tail01"] = std::move(tail);

    json flat = json::object();
    flat["hi"] = c.flat_hi;
    root["flat01"] = std::move(flat);

    json rough = json::object();
    rough["scale"] = c.rough_scale;
    root["rough01"] = std::move(rough);

    json jitter = json::object();
    jitter["flat_weight"] = c.jitter_flat_weight;
    jitter["rough_weight"] = c.jitter_rough_weight;
    root["jitter01"] = std::move(jitter);

    json hue = json::object();
    hue["base_deg"] = c.hue_base_deg;
    hue["warm_span_deg"] = c.hue_warm_span_deg;
    root["hue"] = std::move(hue);

    json sat = json::object();
    sat["base"] = c.sat_base;
    sat["ton_span"] = c.sat_ton_span;
    root["sat"] = std::move(sat);

    json light = json::object();
    light["base"] = c.light_base;
    light["bright_span"] = c.light_bright_span;
    root["light"] = std::move(light);

    json size = json::object();
    size["base_px"] = c.size_base_px;
    size["loud_span_px"] = c.size_loud_span_px;
    root["size"] = std::move(size);

    json spikes = json::object();
    spikes["threshold"] = c.spike_threshold;
    spikes["count_base"] = c.spike_count_base;
    spikes["count_span"] = c.spike_count_span;
    root["spikes"] = std::move(spikes);

    json silent = json::object();
    silent["hue_deg"] = c.silent_hue_deg;
    silent["sat"] = c.silent_sat;
    silent["light"] = c.silent_light;
    silent["size_px"] = c.silent_size_px;
    root["silent"] = std::move(silent);

    return root.dump(2);
}

} // namespace sp
