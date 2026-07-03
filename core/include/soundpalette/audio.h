#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sp {

// Mono, 48 kHz, float32 samples. Analysis is capped at 30 s (§6); duration_s reflects the full
// source file even when truncated is true.
struct AudioBuffer {
    std::vector<float> samples48k_mono;
    int src_rate = 0;
    int src_channels = 0;
    double duration_s = 0.0;
    bool truncated = false;
};

// EBU R128 integrated loudness + true peak, measured on the AudioBuffer's original gain (§6).
struct Loudness {
    double lufs_i = 0.0;
    double true_peak_db = 0.0;
    bool silent = false;
};

// Decodes path (.wav/.flac/.ogg/.mp3) to mono float32 @ 48 kHz, capped at 30 s of analysis audio.
// Returns std::nullopt and fills err on failure.
std::optional<AudioBuffer> decode_file(const std::filesystem::path& path, std::string& err);

// EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK over buffer.samples48k_mono (§6).
Loudness measure_loudness(const AudioBuffer& buffer);

} // namespace sp
