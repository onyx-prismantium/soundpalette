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

FileEntry process_one(const std::filesystem::path &root, const std::filesystem::path &abs_path) {
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
    entry.visual = map_v1(entry.features, entry.loudness, path_seed(entry.path));

    return entry;
}

double round4(double x) {
    return std::round(x * 10000.0) / 10000.0;
}

const char *kDimNames[7] = {"bright01", "warm01", "ton01", "atk01", "tail01", "loud01", "jitter01"};

} // namespace

Manifest scan_directory(const std::filesystem::path &root, const ScanOptions &options) {
    Manifest manifest;
    manifest.root = root.generic_string();
    manifest.include_meta = options.include_meta;
    manifest.engine_version = kVersionString;

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
    thread_count = std::min<unsigned int>(thread_count, std::max<std::size_t>(targets.size(), 1));
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
            out.push_back(process_one(root, targets[i]));
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
    return process_one(root, abs_path);
}

void recompute_stats(Manifest &manifest) {
    // Stats over the seven lint dimensions, non-error + non-silent files only (§8).
    std::array<std::vector<double>, 7> dim_values;
    for (const FileEntry &e : manifest.files) {
        if (!e.error.empty() || e.loudness.silent) {
            continue;
        }
        std::array<double, 7> dims = mapping_dims(e.features, e.loudness);
        for (int d = 0; d < 7; ++d) {
            dim_values[d].push_back(dims[d]);
        }
    }
    for (int d = 0; d < 7; ++d) {
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

std::string manifest_to_json(const Manifest &manifest) {
    using json = nlohmann::ordered_json;

    json root_obj = json::object();
    root_obj["schema_version"] = manifest.schema_version;
    root_obj["mapping_version"] = manifest.mapping_version;
    if (manifest.include_meta) {
        root_obj["engine_version"] = manifest.engine_version;
    }
    root_obj["root"] = manifest.root;
    root_obj["file_count"] = manifest.files.size();

    json files_arr = json::array();
    for (const FileEntry &e : manifest.files) {
        json fe = json::object();
        fe["path"] = e.path;
        fe["sha256"] = e.sha256;
        fe["error"] = e.error;
        fe["duration_s"] = round4(e.duration_s);
        fe["sample_rate"] = e.sample_rate;
        fe["channels"] = e.channels;
        fe["truncated"] = e.truncated;

        json loudness = json::object();
        loudness["lufs_i"] = std::isfinite(e.loudness.lufs_i) ? round4(e.loudness.lufs_i) : -900.0;
        loudness["true_peak_db"] = round4(e.loudness.true_peak_db);
        loudness["silent"] = e.loudness.silent;
        fe["loudness"] = std::move(loudness);

        json features = json::object();
        features["centroid_hz"] = round4(e.features.centroid_hz);
        features["rolloff85_hz"] = round4(e.features.rolloff85_hz);
        features["flatness"] = round4(e.features.flatness);
        features["zcr"] = round4(e.features.zcr);
        features["attack_s"] = round4(e.features.attack_s);
        features["tail_s"] = round4(e.features.tail_s);
        features["tail_clipped"] = e.features.tail_clipped;
        features["roughness"] = round4(e.features.roughness);
        features["warmth"] = round4(e.features.warmth);
        json bands = json::array();
        for (double b : e.features.bands) {
            bands.push_back(round4(b));
        }
        features["bands"] = std::move(bands);
        fe["features"] = std::move(features);

        json visual = json::object();
        visual["hue_deg"] = round4(e.visual.hue_deg);
        visual["sat"] = round4(e.visual.sat);
        visual["light"] = round4(e.visual.light);
        visual["size_px"] = round4(e.visual.size_px);
        visual["spike01"] = round4(e.visual.spike01);
        visual["spikes"] = e.visual.spikes;
        visual["jitter01"] = round4(e.visual.jitter01);
        visual["tail01"] = round4(e.visual.tail01);
        visual["seed"] = e.visual.seed;
        fe["visual"] = std::move(visual);

        files_arr.push_back(std::move(fe));
    }
    root_obj["files"] = std::move(files_arr);

    json stats_obj = json::object();
    for (int d = 0; d < 7; ++d) {
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

} // namespace sp
