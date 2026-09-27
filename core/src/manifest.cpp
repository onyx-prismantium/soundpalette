#include "soundpalette/manifest.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

extern "C" {
#include "sha256.h" // plain C header (extern/sha256), no C++ guards of its own
}

#include "manifest_json.h"
#include "soundpalette/audio.h"
#include "soundpalette/version.h"

namespace sp {

namespace {

bool has_supported_extension(const std::filesystem::path &p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".wav" || ext == ".flac" || ext == ".ogg" || ext == ".mp3";
}

std::string to_forward_slashes(const std::filesystem::path &p) {
    std::string s = p.generic_string();
    return s;
}

std::string hex_lower(const unsigned char *bytes, std::size_t n) {
    static const char *kHex = "0123456789abcdef";
    std::string out(n * 2, '0');
    for (std::size_t i = 0; i < n; ++i) {
        out[2 * i] = kHex[(bytes[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[bytes[i] & 0xF];
    }
    return out;
}

std::string sha256_of_file(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    SHA256_CTX ctx;
    sha256_init(&ctx);
    if (in) {
        std::vector<unsigned char> chunk(1 << 16);
        while (in) {
            in.read(reinterpret_cast<char *>(chunk.data()),
                    static_cast<std::streamsize>(chunk.size()));
            std::streamsize got = in.gcount();
            if (got > 0) {
                sha256_update(&ctx, chunk.data(), static_cast<size_t>(got));
            }
        }
    }
    unsigned char digest[SHA256_BLOCK_SIZE];
    sha256_final(&ctx, digest);
    return hex_lower(digest, SHA256_BLOCK_SIZE);
}

FileEntry process_one(const std::filesystem::path &root, const std::filesystem::path &abs_path,
                      bool with_psycho) {
    FileEntry entry;
    entry.path = to_forward_slashes(std::filesystem::relative(abs_path, root));
    entry.sha256 = sha256_of_file(abs_path);

    std::string err;
    auto buffer = decode_file(abs_path, err);
    if (!buffer.has_value()) {
        entry.error = err.empty() ? "decode failed" : err;
        return entry;
    }

    entry.duration_s = buffer->duration_s;
    entry.sample_rate = buffer->src_rate;
    entry.channels = buffer->src_channels;
    entry.truncated = buffer->truncated;

    entry.loudness = measure_loudness(*buffer);
    entry.features = extract_features(*buffer, entry.loudness);
    // visual needs the psycho block (v2 dims), so compute it first
    if (with_psycho) {
        entry.psycho = compute_psycho(*buffer, active_mapping_config());
    }
    entry.visual = map_v2(entry.features, entry.loudness, entry.psycho, path_seed(entry.path));

    return entry;
}

double round4(double x) {
    return std::round(x * 10000.0) / 10000.0;
}

const char *kDimNames[8] = {"bright01", "warm01", "ton01",    "atk01",
                            "tail01",   "loud01", "jitter01", "fluct01"};

} // namespace

Manifest scan_directory(const std::filesystem::path &root, const ScanOptions &options) {
    Manifest manifest;
    manifest.root = root.generic_string();
    manifest.include_meta = options.include_meta;
    manifest.engine_version = kVersionString;
    manifest.ref_spl = active_mapping_config().ref_spl;
    manifest.mapping_version = active_mapping_config().mapping_version;

    std::vector<std::filesystem::path> targets;
    if (std::filesystem::exists(root) && std::filesystem::is_directory(root)) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && has_supported_extension(entry.path())) {
                targets.push_back(entry.path());
            }
        }
    }

    const unsigned int hw = std::thread::hardware_concurrency();
    unsigned int thread_count =
        options.threads > 0 ? static_cast<unsigned int>(options.threads) : (hw > 0 ? hw : 1);
    const std::size_t target_cap = std::max<std::size_t>(targets.size(), 1);
    if (static_cast<std::size_t>(thread_count) > target_cap) {
        thread_count = static_cast<unsigned int>(target_cap);
    }
    thread_count = std::max<unsigned int>(thread_count, 1);

    std::atomic<std::size_t> next_index{0};
    std::atomic<std::size_t> done_count{0};
    std::vector<std::vector<FileEntry>> per_thread(thread_count);

    auto worker = [&](unsigned int worker_id) {
        std::vector<FileEntry> &out = per_thread[worker_id];
        for (;;) {
            std::size_t i = next_index.fetch_add(1);
            if (i >= targets.size()) {
                break;
            }
            out.push_back(process_one(root, targets[i], options.with_psycho));
            if (options.on_progress) {
                options.on_progress(done_count.fetch_add(1) + 1, targets.size());
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (unsigned int t = 0; t < thread_count; ++t) {
        workers.emplace_back(worker, t);
    }
    for (auto &th : workers) {
        th.join();
    }

    for (auto &bucket : per_thread) {
        for (auto &e : bucket) {
            manifest.files.push_back(std::move(e));
        }
    }

    std::sort(manifest.files.begin(), manifest.files.end(),
              [](const FileEntry &a, const FileEntry &b) { return a.path < b.path; });

    recompute_stats(manifest);

    return manifest;
}

bool has_supported_audio_extension(const std::filesystem::path &path) {
    return has_supported_extension(path);
}

FileEntry analyze_file(const std::filesystem::path &root, const std::filesystem::path &abs_path) {
    return process_one(root, abs_path, true);
}

void recompute_stats(Manifest &manifest) {
    // Stats over the seven lint dimensions, non-error + non-silent files only (§8).
    std::array<std::vector<double>, 8> dim_values;
    for (const FileEntry &e : manifest.files) {
        if (!e.error.empty() || e.loudness.silent) {
            continue;
        }
        std::array<double, 8> dims = mapping_dims(e.features, e.loudness, e.psycho);
        for (int d = 0; d < 8; ++d) {
            dim_values[d].push_back(dims[d]);
        }
    }
    for (int d = 0; d < 8; ++d) {
        const std::vector<double> &v = dim_values[d];
        DimStats s;
        if (!v.empty()) {
            double sum = 0.0;
            s.min = v.front();
            s.max = v.front();
            for (double x : v) {
                sum += x;
                s.min = std::min(s.min, x);
                s.max = std::max(s.max, x);
            }
            s.mean = sum / v.size();
            double var_sum = 0.0;
            for (double x : v) {
                double d0 = x - s.mean;
                var_sum += d0 * d0;
            }
            s.std = std::sqrt(var_sum / v.size());
        }
        manifest.stats[static_cast<std::size_t>(d)] = s;
    }
}

nlohmann::ordered_json file_entry_to_json(const FileEntry &e, bool rounded) {
    using json = nlohmann::ordered_json;
    auto r = [rounded](double x) { return rounded ? round4(x) : x; };
    json fe = json::object();
    fe["path"] = e.path;
    fe["sha256"] = e.sha256;
    fe["error"] = e.error;
    fe["duration_s"] = r(e.duration_s);
    fe["sample_rate"] = e.sample_rate;
    fe["channels"] = e.channels;
    fe["truncated"] = e.truncated;

    json loudness = json::object();
    loudness["lufs_i"] = std::isfinite(e.loudness.lufs_i) ? r(e.loudness.lufs_i) : -900.0;
    loudness["true_peak_db"] = r(e.loudness.true_peak_db);
    loudness["silent"] = e.loudness.silent;
    fe["loudness"] = std::move(loudness);

    json features = json::object();
    features["centroid_hz"] = r(e.features.centroid_hz);
    features["rolloff85_hz"] = r(e.features.rolloff85_hz);
    features["flatness"] = r(e.features.flatness);
    features["zcr"] = r(e.features.zcr);
    features["attack_s"] = r(e.features.attack_s);
    features["tail_s"] = r(e.features.tail_s);
    features["tail_clipped"] = e.features.tail_clipped;
    features["roughness"] = r(e.features.roughness);
    features["warmth"] = r(e.features.warmth);
    json bands = json::array();
    for (double b : e.features.bands) {
        bands.push_back(r(b));
    }
    features["bands"] = std::move(bands);
    fe["features"] = std::move(features);

    // Extension-3 §6: psychoacoustic block (schema 2), original-gain + ref_spl semantics.
    json psycho = json::object();
    psycho["ref_spl"] = r(e.psycho.ref_spl);
    psycho["sones_n5"] = r(e.psycho.sones_n5);
    psycho["sones_mean"] = r(e.psycho.sones_mean);
    psycho["sharpness_acum"] = r(e.psycho.sharpness_acum);
    psycho["roughness_asper"] = r(e.psycho.roughness_asper);
    psycho["fluctuation_vacil"] = r(e.psycho.fluctuation_vacil);
    psycho["experimental_fluctuation"] = e.psycho.experimental_fluctuation;
    fe["psycho"] = std::move(psycho);

    json visual = json::object();
    visual["hue_deg"] = r(e.visual.hue_deg);
    visual["sat"] = r(e.visual.sat);
    visual["light"] = r(e.visual.light);
    visual["size_px"] = r(e.visual.size_px);
    visual["ton01"] = r(e.visual.ton01);
    visual["spike01"] = r(e.visual.spike01);
    visual["spikes"] = e.visual.spikes;
    visual["jitter01"] = r(e.visual.jitter01);
    visual["tail01"] = r(e.visual.tail01);
    visual["fluct01"] = r(e.visual.fluct01);
    visual["loud01"] = r(e.visual.loud01);
    visual["sharp01"] = r(e.visual.sharp01);
    visual["silent"] = e.visual.silent;
    visual["seed"] = e.visual.seed;
    fe["visual"] = std::move(visual);
    return fe;
}

std::string manifest_to_json(const Manifest &manifest) {
    using json = nlohmann::ordered_json;

    json root_obj = json::object();
    root_obj["schema_version"] = manifest.schema_version;
    root_obj["ref_spl"] = round4(manifest.ref_spl);
    root_obj["mapping_version"] = manifest.mapping_version;
    if (manifest.include_meta) {
        root_obj["engine_version"] = manifest.engine_version;
    }
    root_obj["root"] = manifest.root;
    root_obj["file_count"] = manifest.files.size();

    json files_arr = json::array();
    for (const FileEntry &e : manifest.files) {
        json fe = file_entry_to_json(e, true);
        files_arr.push_back(std::move(fe));
    }
    root_obj["files"] = std::move(files_arr);

    json stats_obj = json::object();
    for (int d = 0; d < 8; ++d) {
        const DimStats &s = manifest.stats[static_cast<std::size_t>(d)];
        json dim = json::object();
        dim["mean"] = round4(s.mean);
        dim["std"] = round4(s.std);
        dim["min"] = round4(s.min);
        dim["max"] = round4(s.max);
        stats_obj[kDimNames[d]] = std::move(dim);
    }
    root_obj["stats"] = std::move(stats_obj);

    return root_obj.dump(2);
}

bool file_entry_from_json(const nlohmann::json &j, FileEntry &out, std::string &err) {
    try {
        FileEntry e;
        e.path = j.at("path").get<std::string>();
        e.sha256 = j.value("sha256", "");
        e.error = j.value("error", "");
        e.duration_s = j.value("duration_s", 0.0);
        e.sample_rate = j.value("sample_rate", 0);
        e.channels = j.value("channels", 0);
        e.truncated = j.value("truncated", false);
        if (j.contains("loudness")) {
            const auto &lj = j.at("loudness");
            e.loudness.lufs_i = lj.value("lufs_i", 0.0);
            if (e.loudness.lufs_i <= -900.0) {
                e.loudness.lufs_i = -HUGE_VAL;
            }
            e.loudness.true_peak_db = lj.value("true_peak_db", 0.0);
            e.loudness.silent = lj.value("silent", false);
        }
        if (j.contains("features")) {
            const auto &fj = j.at("features");
            e.features.centroid_hz = fj.value("centroid_hz", 0.0);
            e.features.rolloff85_hz = fj.value("rolloff85_hz", 0.0);
            e.features.flatness = fj.value("flatness", 0.0);
            e.features.zcr = fj.value("zcr", 0.0);
            e.features.attack_s = fj.value("attack_s", 0.0);
            e.features.tail_s = fj.value("tail_s", 0.0);
            e.features.tail_clipped = fj.value("tail_clipped", false);
            e.features.roughness = fj.value("roughness", 0.0);
            e.features.warmth = fj.value("warmth", 0.0);
            if (fj.contains("bands")) {
                const auto &bands = fj.at("bands");
                for (std::size_t k = 0; k < e.features.bands.size() && k < bands.size(); ++k) {
                    e.features.bands[k] = bands[k].get<double>();
                }
            }
        }
        if (j.contains("psycho")) {
            const auto &pj = j.at("psycho");
            e.psycho.ref_spl = pj.value("ref_spl", 0.0);
            e.psycho.sones_n5 = pj.value("sones_n5", 0.0);
            e.psycho.sones_mean = pj.value("sones_mean", 0.0);
            e.psycho.sharpness_acum = pj.value("sharpness_acum", 0.0);
            e.psycho.roughness_asper = pj.value("roughness_asper", 0.0);
            e.psycho.fluctuation_vacil = pj.value("fluctuation_vacil", 0.0);
            e.psycho.experimental_fluctuation = pj.value("experimental_fluctuation", true);
        }
        if (j.contains("visual")) {
            const auto &vj = j.at("visual");
            e.visual.hue_deg = vj.value("hue_deg", 0.0);
            e.visual.sat = vj.value("sat", 0.0);
            e.visual.light = vj.value("light", 0.0);
            e.visual.size_px = vj.value("size_px", 0.0);
            e.visual.ton01 = vj.value("ton01", 0.0);
            e.visual.spike01 = vj.value("spike01", 0.0);
            e.visual.spikes = vj.value("spikes", 0);
            e.visual.jitter01 = vj.value("jitter01", 0.0);
            e.visual.tail01 = vj.value("tail01", 0.0);
            e.visual.fluct01 = vj.value("fluct01", 0.0);
            e.visual.loud01 = vj.value("loud01", 0.0);
            e.visual.sharp01 = vj.value("sharp01", 0.0);
            e.visual.silent = vj.value("silent", false);
            e.visual.seed = vj.value("seed", static_cast<std::uint64_t>(0));
        }
        out = std::move(e);
        return true;
    } catch (const std::exception &ex) {
        err = ex.what();
        return false;
    }
}

std::string file_entry_to_json_string(const FileEntry &e, bool rounded) {
    return file_entry_to_json(e, rounded).dump();
}

std::optional<FileEntry> file_entry_from_json_string(const std::string &text, std::string &err) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception &ex) {
        err = ex.what();
        return std::nullopt;
    }
    FileEntry e;
    if (!file_entry_from_json(j, e, err)) {
        return std::nullopt;
    }
    return e;
}

std::optional<Manifest> manifest_from_json(const std::string &text, std::string &err) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception &ex) {
        err = ex.what();
        return std::nullopt;
    }
    Manifest m;
    try {
        m.schema_version = j.value("schema_version", 1);
        m.ref_spl = j.value("ref_spl", 75.0);
        m.mapping_version = j.value("mapping_version", 1);
        m.include_meta = j.contains("engine_version");
        m.engine_version = j.value("engine_version", "");
        m.root = j.value("root", "");
        for (const auto &fj : j.at("files")) {
            FileEntry e;
            if (!file_entry_from_json(fj, e, err)) {
                return std::nullopt;
            }
            m.files.push_back(std::move(e));
        }
    } catch (const std::exception &ex) {
        err = ex.what();
        return std::nullopt;
    }
    std::sort(m.files.begin(), m.files.end(),
              [](const FileEntry &a, const FileEntry &b) { return a.path < b.path; });
    recompute_stats(m);
    return m;
}

} // namespace sp
