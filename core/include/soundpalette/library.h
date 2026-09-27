#pragma once

// The sound library (extension-4 §5/§6): a persistent per-root SQLite index holding every
// file's analysis (FileEntry, canonical JSON shape) plus its UCS annotation, with FTS5 search.
// Analysis stays model-free and deterministic; annotations are additive metadata and never
// touch the manifest. Human edits (source "human") are locked and never overwritten.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "soundpalette/manifest.h"

struct sqlite3;

namespace sp {

// §6.2 provenance, in precedence order (higher wins on write).
enum class AnnotationSource { kFolder = 1, kFilename = 2, kMetadata = 3, kModel = 4, kHuman = 5 };
const char *annotation_source_name(AnnotationSource s);
std::optional<AnnotationSource> annotation_source_from_name(std::string_view name);

struct Annotation {
    std::string cat_id;      // UCS CatID or empty (= unannotated)
    std::string fx_name;     // UCS FXName (2-6 words, Title Case)
    std::string description; // one sentence <= 140 chars
    std::vector<std::string> keywords;
    double confidence = 0.0; // [0,1]
    AnnotationSource source = AnnotationSource::kFilename;
    std::string model;           // endpoint-reported model id (source model only)
    std::string prompt_version;  // e.g. "annotate_v1" (source model only)
    std::string annotated_at;    // ISO-8601 UTC; filled on write when empty
    bool locked = false;         // human writes imply locked
    std::string candidates_json; // §7 stage-2 shortlist, for the inspector (may be empty)
};

struct LibraryRow {
    std::int64_t id = 0;
    std::string path; // relative to root, forward slashes
    std::string sha256;
    std::int64_t size = 0;
    std::int64_t mtime_ns = 0;
    bool present = true;
    FileEntry entry;
    std::optional<Annotation> annotation;
};

struct UpdateOptions {
    int threads = 0;    // 0 => hardware_concurrency
    bool prune = false; // delete rows of files missing from disk (else marked present=0)
    std::function<void(std::size_t, std::size_t)> on_progress; // (done, total) analyzed files
};

struct UpdateReport {
    std::size_t added = 0;
    std::size_t changed = 0;
    std::size_t moved = 0;
    std::size_t unchanged = 0;
    std::size_t returned = 0; // was missing, back on disk unchanged
    std::size_t missing = 0;  // present=0 after this update
    std::size_t pruned = 0;
    std::size_t analyzed = 0;    // files that went through the analysis pipeline
    std::size_t errors = 0;      // analyzed files whose decode failed
    bool reanalyzed_all = false; // engine/mapping/ref_spl mismatch forced a full re-analysis
};

struct SearchQuery {
    std::string text;             // free text -> FTS5 (prefix on the last token); empty = no FTS
    std::string cat_id_glob;      // SQL GLOB on cat_id, e.g. "AMB*"
    std::string category;         // exact UCS Category, e.g. "AMBIENCE"
    bool unannotated = false;     // only rows without a cat_id
    double min_confidence = -1.0; // < 0 = no filter
    int limit = 100;              // <= 0 = no limit
    bool include_missing = false;
};

enum class SetResult { kWritten, kSkippedLocked, kSkippedLowerPrecedence, kUnknownPath };

struct ClassifyReport {
    std::size_t examined = 0;
    std::size_t written = 0;
    std::size_t skipped_locked = 0;
    std::size_t skipped_precedence = 0;
    std::size_t unmatched = 0;
};

class Library {
public:
    static constexpr const char *kDirName = ".soundpalette";
    static constexpr const char *kIndexName = "library.sqlite";
    static constexpr int kSchemaVersion = 1;

    static std::filesystem::path index_path(const std::filesystem::path &root);
    // Walks up from `start` (a file or directory) to the nearest directory holding an index.
    static std::optional<std::filesystem::path> find_root(const std::filesystem::path &start);

    // Creates a new index (refuses if one exists at root or in any ancestor, §5.1).
    static std::unique_ptr<Library> create(const std::filesystem::path &root, std::string &err);
    static std::unique_ptr<Library> open(const std::filesystem::path &root, std::string &err);
    ~Library();
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;

    const std::filesystem::path &root() const {
        return root_;
    }
    std::string meta(std::string_view key) const;
    void set_meta(std::string_view key, std::string_view value);

    // §5.3 incremental ingest.
    UpdateReport update(const UpdateOptions &options, std::string &err);
    std::size_t prune();

    std::optional<LibraryRow> get(std::string_view rel_path) const;
    std::vector<LibraryRow> search(const SearchQuery &query, std::string &err) const;
    std::size_t count_files(bool present_only = true) const;
    std::size_t count_annotated() const;
    // (category, count) over present annotated rows, CSV category order; for the GUI tree.
    std::vector<std::pair<std::string, std::size_t>> category_counts() const;
    std::vector<std::pair<std::string, std::size_t>> cat_id_counts(std::string_view category) const;

    // §6.2 precedence-checked write. `force` overrides the precedence rule only; a locked row
    // is written by a human-source annotation and nothing else.
    SetResult set_annotation(std::string_view rel_path, const Annotation &annotation, bool force);
    bool clear_annotation(std::string_view rel_path);
    bool set_locked(std::string_view rel_path, bool locked);

    // §8 offline pass over every present row (filename grammar, then token match).
    ClassifyReport classify_offline(bool force);

    // A canonical manifest of the matching rows (all present rows when query is null); root
    // is written as given so `scan <root>` and `library export <root>` produce identical bytes.
    Manifest export_manifest(const SearchQuery *query, const std::string &root_as_given,
                             std::string &err) const;

    sqlite3 *raw() const {
        return db_;
    }

private:
    Library(sqlite3 *db, std::filesystem::path root);
    bool init_schema(std::string &err);
    void refresh_fts(std::int64_t file_id);
    std::optional<LibraryRow> row_by_id(std::int64_t id) const;

    sqlite3 *db_ = nullptr;
    std::filesystem::path root_;
};

// §8: the pure offline classifier for one relative path. nullopt = no confident match.
std::optional<Annotation> classify_offline_path(std::string_view rel_path);

// Current time as ISO-8601 UTC ("2026-09-27T16:04:05Z").
std::string iso8601_utc_now();

} // namespace sp
