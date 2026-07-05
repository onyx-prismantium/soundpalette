#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "soundpalette/audio.h"
#include "soundpalette/features.h"
#include "soundpalette/psycho.h"

namespace sp {

// Correction operates on the original audio: full length, native sample rate, all channels
// (extension §6.1). Planar float storage; per-channel processing with linked gains/envelopes.
struct NativeAudio {
    std::vector<std::vector<float>> channels;
    int rate = 0;
    std::size_t frame_count() const {
        return channels.empty() ? 0 : channels[0].size();
    }
};

// One recipe op (extension §6.2). Tagged union kept flat for simple JSON round-tripping.
enum class OpType { kGainDb, kGainToLufs, kLowShelf, kHighShelf, kAttackSoften, kTailShorten };

struct Op {
    OpType op = OpType::kGainDb;
    double db = 0.0;                                  // gain_db
    double target_lufs = -23.0, tp_ceiling_db = -1.0; // gain_to_lufs
    double freq_hz = 0.0, gain_db = 0.0, q = 0.707;   // low_shelf / high_shelf
    double fade_ms = 0.0;                             // attack_soften
    double target_tail_s = 0.0;                       // tail_shorten
};

const char *op_name(OpType op);

// Per-chain application report (extension §6.2 gain_to_lufs notes).
struct ApplyReport {
    bool limited_by_peak = false;
};

// Recipe document (extension §6.3, recipe_version 1). result_* fields mirror the schema's
// "result" block and are filled by the proposer / harmonize.
struct Recipe {
    int recipe_version = 1;
    std::string source_path;
    std::string source_sha256;
    std::string target_baseline;
    int target_mapping_version = 1;
    double target_threshold = 2.5;
    std::vector<Op> ops;

    int result_iterations = 0;
    bool result_converged = false;
    bool result_limited_by_peak = false;
    std::vector<std::string> result_unresolved;
    double result_max_z_before = 0.0;
    double result_max_z_after = 0.0;
    std::map<std::string, double> result_dims_before; // offending dims only
    std::map<std::string, double> result_dims_after;
};

// Decode to native rate/channels, full length (no 30 s cap, no downmix) — extension §6.1.
std::optional<NativeAudio> decode_file_native(const std::filesystem::path &path, std::string &err);

// Output container: WAV, 32-bit float, native rate/channels (extension §6.1).
bool write_wav_f32(const std::filesystem::path &path, const NativeAudio &audio, std::string &err);

// Chain order rule (extension §6.2): gain_to_lufs must be last if present.
bool validate_ops(const std::vector<Op> &ops, std::string &err);

// Applies the ops in order, in place. Coefficients and filter state in double (§6.1).
void apply_chain(NativeAudio &audio, const std::vector<Op> &ops, ApplyReport &report);

// Runs the standard PLAN.md §6 analysis pipeline over in-memory audio (downmix -> 48 kHz ->
// 30 s cap), so proposer iterations measure exactly what a scan of the written file would.
void analyze_native(const NativeAudio &audio, Loudness &loudness, Features &features,
                    PsychoFeatures &psycho);

// SHA-256 of a file on disk (recipe provenance, extension §6.3).
std::string file_sha256(const std::filesystem::path &path);

// Canonical JSON (PLAN.md §8 rules: fixed key order, 4-decimal rounding, no timestamps).
std::string recipe_to_json(const Recipe &recipe);
std::optional<Recipe> recipe_from_json(const std::string &text, std::string &err);

} // namespace sp
