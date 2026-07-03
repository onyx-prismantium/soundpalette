#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "soundpalette/mapping.h"

namespace sp {

struct FileEntry {
    std::string path; // relative to scan root, forward slashes
    std::string sha256;
    std::string error; // non-empty if decode failed; other fields are then default
    double duration_s = 0.0;
    int sample_rate = 0;
    int channels = 0;
    bool truncated = false;
    Loudness loudness;
    Features features;
    Visual visual;
};

struct DimStats {
    double mean = 0.0;
    double std = 0.0;
    double min = 0.0;
    double max = 0.0;
};

// Fixed dimension order, matching mapping_dims(): bright01, warm01, ton01, atk01, tail01,
// loud01, jitter01 (§8/§9).
enum class LintDim { kBright01 = 0, kWarm01, kTon01, kAtk01, kTail01, kLoud01, kJitter01, kCount };

struct Manifest {
    int schema_version = 1;
    int mapping_version = 1;
    std::string engine_version;
    std::string root;
    std::vector<FileEntry> files;
    std::array<DimStats, 7> stats{}; // over non-error, non-silent files (§8)
    bool include_meta = true;        // false => manifest_to_json omits engine_version (--no-meta)
};

struct ScanOptions {
    int threads = 0; // 0 => std::thread::hardware_concurrency()
    bool include_meta = true;
};

// Recursively scans root for .wav/.flac/.ogg/.mp3 (case-insensitive). Files are processed by a
// fixed thread pool but the returned Manifest.files is always sorted ascending by path (byte
// order), so output never depends on scheduling (§5).
Manifest scan_directory(const std::filesystem::path& root, const ScanOptions& options);

// Canonical serialization (§8): UTF-8, LF, 2-space indent, fixed key order, files sorted by
// path, floats rounded to 4 decimal places, no timestamps or absolute paths.
std::string manifest_to_json(const Manifest& manifest);

} // namespace sp
