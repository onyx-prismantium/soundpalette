#pragma once

// Model-backed annotation (extension-4 §7): the core never speaks HTTP. It spawns an
// *annotator* command, hands it a 16 kHz mono excerpt plus analysis hints over a JSON-lines
// protocol, and turns the answers into a library Annotation through the deterministic
// two-stage CatID selection of §7.2. CI never runs a model: tests use a mock annotator.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "soundpalette/audio.h"
#include "soundpalette/library.h"
#include "soundpalette/ucs.h"

namespace sp {

inline constexpr int kAnnotatorProtocolVersion = 1;
inline constexpr const char *kDefaultAnnotatorCommand = "soundpalette-annotate";

struct AnnotatorOptions {
    std::string command;      // empty => $SP_ANNOTATOR, else kDefaultAnnotatorCommand
    int inflight = 4;         // max outstanding requests (worker threads)
    double timeout_s = 120.0; // per request; a timeout kills + restarts the annotator
    bool dry_run = false;     // run the model but do not write to the library
    bool force = false;       // overwrite lower-precedence rows (never locked ones)
    std::size_t max_shortlist = 12;
    // (done, total, path, status) — status is "ok", "error: ...", "skipped: ..."
    std::function<void(std::size_t, std::size_t, const std::string &, const std::string &)>
        on_progress;
};

struct AnnotatorInfo {
    std::string name;
    std::string model;
    std::string prompt_version;
    std::vector<std::string> stages;
};

struct AnnotateFileResult {
    std::string path;
    bool ok = false;
    std::string error;  // non-empty when !ok
    std::string status; // "written", "dry-run", "skipped: locked", "skipped: precedence"
    Annotation annotation;
};

struct AnnotateReport {
    std::size_t requested = 0;
    std::size_t annotated = 0; // written (or would be, in a dry run)
    std::size_t skipped_locked = 0;
    std::size_t skipped_precedence = 0;
    std::size_t errors = 0;
    std::size_t timeouts = 0;
    std::size_t restarts = 0;
    std::string fatal; // annotator could not be started at all
    AnnotatorInfo annotator;
    std::vector<AnnotateFileResult> files;
};

// Stage-1 answer (what the model heard).
struct Stage1 {
    std::string description;
    std::string fx_name;
    std::string category; // one of the 82 UCS categories, or anything else if the model erred
    std::vector<std::string> keywords;
    double confidence = 0.0;
};

// §7.2 shortlist: ucs_rank over keywords ∪ fx_name ∪ description tokens within the model's
// category, top `max_n`, padded to at least 3 with the category's first SubCategories. If the
// category is unknown, the whole list is ranked (category-less) instead.
std::vector<UcsMatch> stage2_shortlist(const Stage1 &s1, std::size_t max_n = 12);

struct Stage2Decision {
    std::string cat_id; // empty => unresolved (falls back to no CatID)
    double confidence = 0.0;
    bool from_shortlist = false;
    std::string note; // why the model's pick was adjusted, for the report
};

// §7.2 acceptance: a CatID inside the shortlist is taken as is; one outside it is accepted only
// if it exists and its Category matches the shortlist's; otherwise the top shortlist entry is
// used with confidence halved. An empty shortlist yields an empty CatID.
Stage2Decision resolve_stage2(const std::vector<UcsMatch> &shortlist,
                              const std::string &model_cat_id, double model_confidence);

// The model's excerpt: 16 kHz mono 16-bit PCM WAV from the analysis buffer (≤ 30 s), windowed-
// sinc decimation from 48 kHz. false + err on write failure.
bool write_wav_16k_mono(const AudioBuffer &buffer, const std::filesystem::path &path,
                        std::string &err);

// Runs the annotator over the given library-relative paths and writes source "model"
// annotations (precedence rules of §6.2 apply). Never throws; problems land in the report.
AnnotateReport annotate_paths(Library &library, const std::vector<std::string> &rel_paths,
                              const AnnotatorOptions &options);

// Resolves the effective annotator argv (option, $SP_ANNOTATOR, default).
std::vector<std::string> annotator_argv(const AnnotatorOptions &options);

} // namespace sp
