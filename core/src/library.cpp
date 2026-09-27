#include "soundpalette/library.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "sqlite3.h"

#include "manifest_json.h"
#include "soundpalette/mapping.h"
#include "soundpalette/recipe.h"
#include "soundpalette/ucs.h"
#include "soundpalette/version.h"

namespace sp {

namespace {

// Minimal RAII statement wrapper; every SQL string is a literal or built from validated parts.
class Stmt {
public:
    Stmt(sqlite3 *db, const std::string &sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
            std::string msg = sqlite3_errmsg(db);
            throw std::runtime_error("sqlite prepare failed: " + msg + " [" + sql + "]");
        }
    }
    ~Stmt() {
        sqlite3_finalize(stmt_);
    }
    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;

    Stmt &bind(int i, std::string_view s) {
        sqlite3_bind_text(stmt_, i, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt &bind(int i, std::int64_t v) {
        sqlite3_bind_int64(stmt_, i, v);
        return *this;
    }
    Stmt &bind(int i, int v) {
        sqlite3_bind_int(stmt_, i, v);
        return *this;
    }
    Stmt &bind(int i, double v) {
        sqlite3_bind_double(stmt_, i, v);
        return *this;
    }
    Stmt &bind_null(int i) {
        sqlite3_bind_null(stmt_, i);
        return *this;
    }
    bool step() {
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) {
            return true;
        }
        if (rc == SQLITE_DONE) {
            return false;
        }
        throw std::runtime_error(std::string("sqlite step failed: ") +
                                 sqlite3_errmsg(sqlite3_db_handle(stmt_)));
    }
    void reset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
    std::string text(int col) const {
        const unsigned char *t = sqlite3_column_text(stmt_, col);
        return t ? reinterpret_cast<const char *>(t) : std::string();
    }
    bool is_null(int col) const {
        return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
    }
    std::int64_t i64(int col) const {
        return sqlite3_column_int64(stmt_, col);
    }
    double f64(int col) const {
        return sqlite3_column_double(stmt_, col);
    }

private:
    sqlite3_stmt *stmt_ = nullptr;
};

void exec(sqlite3 *db, const char *sql) {
    char *msg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        std::string m = msg ? msg : "unknown";
        sqlite3_free(msg);
        throw std::runtime_error("sqlite exec failed: " + m);
    }
}

const char *kSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS files(
  id INTEGER PRIMARY KEY,
  path TEXT UNIQUE NOT NULL,
  sha256 TEXT NOT NULL,
  size INTEGER NOT NULL DEFAULT 0,
  mtime_ns INTEGER NOT NULL DEFAULT 0,
  duration_s REAL NOT NULL DEFAULT 0,
  sample_rate INTEGER NOT NULL DEFAULT 0,
  channels INTEGER NOT NULL DEFAULT 0,
  error TEXT NOT NULL DEFAULT '',
  entry_json TEXT NOT NULL,
  present INTEGER NOT NULL DEFAULT 1
);
CREATE INDEX IF NOT EXISTS files_sha ON files(sha256);
CREATE TABLE IF NOT EXISTS annotations(
  file_id INTEGER PRIMARY KEY REFERENCES files(id) ON DELETE CASCADE,
  cat_id TEXT NOT NULL DEFAULT '',
  fx_name TEXT NOT NULL DEFAULT '',
  description TEXT NOT NULL DEFAULT '',
  keywords TEXT NOT NULL DEFAULT '[]',
  confidence REAL NOT NULL DEFAULT 0,
  source TEXT NOT NULL,
  model TEXT NOT NULL DEFAULT '',
  prompt_version TEXT NOT NULL DEFAULT '',
  annotated_at TEXT NOT NULL DEFAULT '',
  locked INTEGER NOT NULL DEFAULT 0,
  candidates_json TEXT NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS annotations_cat ON annotations(cat_id);
CREATE VIRTUAL TABLE IF NOT EXISTS search USING fts5(
  path_tokens, fx_name, description, keywords, cat_id, category, sub_category,
  tokenize='unicode61'
);
)sql";

std::string join_tokens(const std::vector<std::string> &tokens) {
    std::string out;
    for (const std::string &t : tokens) {
        if (!out.empty()) {
            out += ' ';
        }
        out += t;
    }
    return out;
}

std::string keywords_to_json(const std::vector<std::string> &kw) {
    nlohmann::json j = nlohmann::json::array();
    for (const std::string &k : kw) {
        j.push_back(k);
    }
    return j.dump();
}

std::vector<std::string> keywords_from_json(const std::string &text) {
    std::vector<std::string> out;
    try {
        nlohmann::json j = nlohmann::json::parse(text);
        if (j.is_array()) {
            for (const auto &k : j) {
                if (k.is_string()) {
                    out.push_back(k.get<std::string>());
                }
            }
        }
    } catch (const std::exception &) {
    }
    return out;
}

// FTS5 query from free text: every token quoted (so user punctuation can never reach the
// query parser), implicit AND, prefix match on the last token.
std::string fts_query_from_text(const std::string &text) {
    std::vector<std::string> tokens = ucs_tokenize(text);
    std::string q;
    for (std::size_t k = 0; k < tokens.size(); ++k) {
        if (k > 0) {
            q += ' ';
        }
        q += '"';
        q += tokens[k];
        q += '"';
        if (k + 1 == tokens.size()) {
            q += '*';
        }
    }
    return q;
}

std::int64_t mtime_ns_of(const std::filesystem::path &p) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec) {
        return 0;
    }
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
}

std::int64_t size_of(const std::filesystem::path &p) {
    std::error_code ec;
    auto s = std::filesystem::file_size(p, ec);
    return ec ? 0 : static_cast<std::int64_t>(s);
}

Annotation annotation_from_row(const Stmt &s, int base) {
    // columns: cat_id, fx_name, description, keywords, confidence, source, model,
    // prompt_version, annotated_at, locked, candidates_json
    Annotation a;
    a.cat_id = s.text(base + 0);
    a.fx_name = s.text(base + 1);
    a.description = s.text(base + 2);
    a.keywords = keywords_from_json(s.text(base + 3));
    a.confidence = s.f64(base + 4);
    a.source = annotation_source_from_name(s.text(base + 5)).value_or(AnnotationSource::kFilename);
    a.model = s.text(base + 6);
    a.prompt_version = s.text(base + 7);
    a.annotated_at = s.text(base + 8);
    a.locked = s.i64(base + 9) != 0;
    a.candidates_json = s.text(base + 10);
    return a;
}

const char *kRowColumns =
    "f.id, f.path, f.sha256, f.size, f.mtime_ns, f.present, f.entry_json, "
    "a.cat_id, a.fx_name, a.description, a.keywords, a.confidence, a.source, a.model, "
    "a.prompt_version, a.annotated_at, a.locked, a.candidates_json";

LibraryRow row_from_stmt(const Stmt &s) {
    LibraryRow r;
    r.id = s.i64(0);
    r.path = s.text(1);
    r.sha256 = s.text(2);
    r.size = s.i64(3);
    r.mtime_ns = s.i64(4);
    r.present = s.i64(5) != 0;
    std::string err;
    auto e = file_entry_from_json_string(s.text(6), err);
    if (e) {
        r.entry = std::move(*e);
    } else {
        r.entry.path = r.path;
        r.entry.error = "index: " + err;
    }
    if (!s.is_null(12)) { // a.source non-null => annotation row exists
        r.annotation = annotation_from_row(s, 7);
    }
    return r;
}

std::string to_forward(const std::filesystem::path &p) {
    return p.generic_string();
}

} // namespace

const char *annotation_source_name(AnnotationSource s) {
    switch (s) {
    case AnnotationSource::kFolder:
        return "folder";
    case AnnotationSource::kFilename:
        return "filename";
    case AnnotationSource::kMetadata:
        return "metadata";
    case AnnotationSource::kModel:
        return "model";
    case AnnotationSource::kHuman:
        return "human";
    }
    return "filename";
}

std::optional<AnnotationSource> annotation_source_from_name(std::string_view name) {
    if (name == "folder") {
        return AnnotationSource::kFolder;
    }
    if (name == "filename") {
        return AnnotationSource::kFilename;
    }
    if (name == "metadata") {
        return AnnotationSource::kMetadata;
    }
    if (name == "model") {
        return AnnotationSource::kModel;
    }
    if (name == "human") {
        return AnnotationSource::kHuman;
    }
    return std::nullopt;
}

std::string iso8601_utc_now() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", (tm.tm_year + 1900) % 10000,
                  (tm.tm_mon + 1) % 100, tm.tm_mday % 100, tm.tm_hour % 100, tm.tm_min % 100,
                  tm.tm_sec % 100);
    return buf;
}

// ---------------------------------------------------------------------------------------------

Library::Library(sqlite3 *db, std::filesystem::path root) : db_(db), root_(std::move(root)) {}

Library::~Library() {
    if (db_) {
        sqlite3_close(db_);
    }
}

std::filesystem::path Library::index_path(const std::filesystem::path &root) {
    return root / kDirName / kIndexName;
}

std::optional<std::filesystem::path> Library::find_root(const std::filesystem::path &start) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(start, ec);
    if (ec) {
        return std::nullopt;
    }
    p = p.lexically_normal();
    if (!std::filesystem::is_directory(p, ec)) {
        p = p.parent_path();
    }
    while (!p.empty()) {
        if (std::filesystem::exists(index_path(p), ec)) {
            return p;
        }
        std::filesystem::path parent = p.parent_path();
        if (parent == p) {
            break;
        }
        p = parent;
    }
    return std::nullopt;
}

bool Library::init_schema(std::string &err) {
    try {
        exec(db_, "PRAGMA journal_mode=WAL;");
        exec(db_, "PRAGMA foreign_keys=ON;");
        exec(db_, "PRAGMA synchronous=NORMAL;");
        exec(db_, kSchemaSql);
        std::string sv = meta("schema_version");
        if (sv.empty()) {
            set_meta("schema_version", std::to_string(kSchemaVersion));
            set_meta("created_at", iso8601_utc_now());
        } else if (std::stoi(sv) > kSchemaVersion) {
            err = "library index schema " + sv + " is newer than this engine supports";
            return false;
        }
        set_meta("ucs_version", ucs_version());
        return true;
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
}

std::unique_ptr<Library> Library::create(const std::filesystem::path &root_in, std::string &err) {
    std::error_code ec;
    std::filesystem::path root = std::filesystem::absolute(root_in, ec).lexically_normal();
    if (!std::filesystem::is_directory(root, ec)) {
        err = "not a directory: " + root_in.string();
        return nullptr;
    }
    if (auto existing = find_root(root)) {
        err = "a library already exists at " + existing->string() + " (roots never nest)";
        return nullptr;
    }
    std::filesystem::create_directories(root / kDirName, ec);
    if (ec) {
        err = "cannot create " + (root / kDirName).string();
        return nullptr;
    }
    return open(root, err);
}

std::unique_ptr<Library> Library::open(const std::filesystem::path &root_in, std::string &err) {
    std::error_code ec;
    std::filesystem::path root = std::filesystem::absolute(root_in, ec).lexically_normal();
    std::filesystem::path idx = index_path(root);
    if (!std::filesystem::exists(root / kDirName, ec)) {
        err = "no library at " + root_in.string() + " (run: soundpalette library init <dir>)";
        return nullptr;
    }
    sqlite3 *db = nullptr;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(idx.string().c_str(), &db, flags, nullptr) != SQLITE_OK) {
        err = std::string("cannot open ") + idx.string() + ": " + (db ? sqlite3_errmsg(db) : "");
        if (db) {
            sqlite3_close(db);
        }
        return nullptr;
    }
    sqlite3_busy_timeout(db, 5000);
    std::unique_ptr<Library> lib(new Library(db, root));
    if (!lib->init_schema(err)) {
        return nullptr;
    }
    return lib;
}

std::string Library::meta(std::string_view key) const {
    Stmt s(db_, "SELECT value FROM meta WHERE key = ?1");
    s.bind(1, key);
    return s.step() ? s.text(0) : std::string();
}

void Library::set_meta(std::string_view key, std::string_view value) {
    Stmt s(db_, "INSERT INTO meta(key, value) VALUES(?1, ?2) "
                "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    s.bind(1, key).bind(2, value);
    s.step();
}

void Library::refresh_fts(std::int64_t file_id) {
    {
        Stmt del(db_, "DELETE FROM search WHERE rowid = ?1");
        del.bind(1, file_id);
        del.step();
    }
    Stmt sel(db_, "SELECT f.path, f.present, a.fx_name, a.description, a.keywords, a.cat_id "
                  "FROM files f LEFT JOIN annotations a ON a.file_id = f.id WHERE f.id = ?1");
    sel.bind(1, file_id);
    if (!sel.step()) {
        return;
    }
    std::string path = sel.text(0);
    std::string fx = sel.text(2);
    std::string desc = sel.text(3);
    std::string kw = join_tokens(keywords_from_json(sel.text(4)));
    std::string cat_id = sel.text(5);
    std::string category;
    std::string sub;
    if (const UcsEntry *e = ucs_find(cat_id)) {
        category = std::string(e->category);
        sub = std::string(e->sub_category);
    }
    Stmt ins(db_, "INSERT INTO search(rowid, path_tokens, fx_name, description, keywords, "
                  "cat_id, category, sub_category) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
    ins.bind(1, file_id)
        .bind(2, join_tokens(ucs_tokenize(path)))
        .bind(3, fx)
        .bind(4, desc)
        .bind(5, kw)
        .bind(6, cat_id)
        .bind(7, category)
        .bind(8, sub);
    ins.step();
}

UpdateReport Library::update(const UpdateOptions &options, std::string &err) {
    UpdateReport report;
    try {
        // 1. Disk walk (skip the index directory).
        struct Disk {
            std::string rel;
            std::filesystem::path abs;
            std::int64_t size;
            std::int64_t mtime;
        };
        std::vector<Disk> disk;
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(root_, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            const auto &de = *it;
            if (de.is_directory(ec) && de.path().filename() == kDirName) {
                it.disable_recursion_pending();
                continue;
            }
            if (de.is_regular_file(ec) && has_supported_audio_extension(de.path())) {
                Disk d;
                d.abs = de.path();
                d.rel = to_forward(std::filesystem::relative(d.abs, root_, ec));
                d.size = size_of(d.abs);
                d.mtime = mtime_ns_of(d.abs);
                disk.push_back(std::move(d));
            }
        }
        std::sort(disk.begin(), disk.end(),
                  [](const Disk &a, const Disk &b) { return a.rel < b.rel; });

        // 2. Existing rows.
        struct Known {
            std::int64_t id;
            std::string sha;
            std::int64_t size;
            std::int64_t mtime;
            bool present;
        };
        std::unordered_map<std::string, Known> known;
        {
            Stmt s(db_, "SELECT id, path, sha256, size, mtime_ns, present FROM files");
            while (s.step()) {
                known.emplace(s.text(1),
                              Known{s.i64(0), s.text(2), s.i64(3), s.i64(4), s.i64(5) != 0});
            }
        }

        const MappingConfig &cfg = active_mapping_config();
        const std::string engine = kVersionString;
        const std::string mapping = std::to_string(cfg.mapping_version);
        char ref_buf[32];
        std::snprintf(ref_buf, sizeof ref_buf, "%.4f", cfg.ref_spl);
        const std::string ref_spl = ref_buf;
        bool reanalyze_all =
            !known.empty() && (meta("engine_version") != engine ||
                               meta("mapping_version") != mapping || meta("ref_spl") != ref_spl);
        report.reanalyzed_all = reanalyze_all;

        // 3. Classify disk files.
        std::vector<const Disk *> to_analyze;
        std::vector<std::pair<const Disk *, std::int64_t>> touch; // row id, new size/mtime
        std::vector<std::pair<const Disk *, std::int64_t>> moved; // old row id -> new path
        std::unordered_set<std::string> on_disk;
        std::unordered_set<std::int64_t> claimed;
        // sha index of rows whose path is no longer on disk (candidates for "moved")
        std::unordered_map<std::string, std::vector<std::pair<std::string, std::int64_t>>> by_sha;
        for (const Disk &d : disk) {
            on_disk.insert(d.rel);
        }
        for (const auto &[path, k] : known) {
            if (!on_disk.count(path)) {
                by_sha[k.sha].push_back({path, k.id});
            }
        }
        for (const Disk &d : disk) {
            auto kit = known.find(d.rel);
            if (kit != known.end()) {
                const Known &k = kit->second;
                if (reanalyze_all) {
                    to_analyze.push_back(&d);
                    continue;
                }
                if (k.size == d.size && k.mtime == d.mtime) {
                    if (k.present) {
                        ++report.unchanged;
                    } else {
                        ++report.returned;
                    }
                    touch.push_back({&d, k.id});
                    continue;
                }
                std::string sha = file_sha256(d.abs);
                if (sha == k.sha) {
                    ++report.unchanged;
                    touch.push_back({&d, k.id});
                } else {
                    ++report.changed;
                    to_analyze.push_back(&d);
                }
                continue;
            }
            // new path: moved?
            std::string sha = file_sha256(d.abs);
            auto mit = by_sha.find(sha);
            if (mit != by_sha.end()) {
                bool found = false;
                for (auto &[old_path, old_id] : mit->second) {
                    if (!claimed.count(old_id)) {
                        claimed.insert(old_id);
                        moved.push_back({&d, old_id});
                        found = true;
                        break;
                    }
                }
                if (found) {
                    ++report.moved;
                    continue;
                }
            }
            ++report.added;
            to_analyze.push_back(&d);
        }

        // 4. Analysis (thread pool, deterministic per file).
        std::vector<FileEntry> results(to_analyze.size());
        if (!to_analyze.empty()) {
            unsigned int hw = std::thread::hardware_concurrency();
            unsigned int threads = options.threads > 0 ? static_cast<unsigned int>(options.threads)
                                                       : (hw > 0 ? hw : 1);
            threads = std::min<unsigned int>(threads, static_cast<unsigned int>(to_analyze.size()));
            threads = std::max<unsigned int>(threads, 1);
            std::atomic<std::size_t> next{0};
            std::atomic<std::size_t> done{0};
            auto worker = [&] {
                for (;;) {
                    std::size_t i = next.fetch_add(1);
                    if (i >= to_analyze.size()) {
                        break;
                    }
                    results[i] = analyze_file(root_, to_analyze[i]->abs);
                    if (options.on_progress) {
                        options.on_progress(done.fetch_add(1) + 1, to_analyze.size());
                    }
                }
            };
            std::vector<std::thread> pool;
            for (unsigned int t = 0; t < threads; ++t) {
                pool.emplace_back(worker);
            }
            for (auto &th : pool) {
                th.join();
            }
        }
        report.analyzed = to_analyze.size();

        // 5. Write everything in one transaction.
        exec(db_, "BEGIN");
        try {
            Stmt touch_s(db_,
                         "UPDATE files SET size = ?2, mtime_ns = ?3, present = 1 WHERE id = ?1");
            for (auto &[d, id] : touch) {
                touch_s.reset();
                touch_s.bind(1, id).bind(2, d->size).bind(3, d->mtime);
                touch_s.step();
            }
            Stmt move_s(db_, "UPDATE files SET path = ?2, size = ?3, mtime_ns = ?4, present = 1, "
                             "entry_json = ?5 WHERE id = ?1");
            Stmt entry_s(db_, "SELECT entry_json FROM files WHERE id = ?1");
            for (auto &[d, id] : moved) {
                entry_s.reset();
                entry_s.bind(1, id);
                std::string ej = entry_s.step() ? entry_s.text(0) : "{}";
                // the stored entry carries the old relative path; rewrite it
                std::string perr;
                auto e = file_entry_from_json_string(ej, perr);
                if (e) {
                    e->path = d->rel;
                    e->visual = map_v2(e->features, e->loudness, e->psycho, path_seed(e->path));
                    ej = file_entry_to_json_string(*e, false);
                }
                move_s.reset();
                move_s.bind(1, id).bind(2, d->rel).bind(3, d->size).bind(4, d->mtime).bind(5, ej);
                move_s.step();
                refresh_fts(id);
            }
            Stmt upsert(db_,
                        "INSERT INTO files(path, sha256, size, mtime_ns, duration_s, sample_rate, "
                        "channels, error, entry_json, present) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, "
                        "?8, ?9, 1) ON CONFLICT(path) DO UPDATE SET sha256 = excluded.sha256, "
                        "size = excluded.size, mtime_ns = excluded.mtime_ns, duration_s = "
                        "excluded.duration_s, sample_rate = excluded.sample_rate, channels = "
                        "excluded.channels, error = excluded.error, entry_json = "
                        "excluded.entry_json, present = 1");
            Stmt id_of(db_, "SELECT id FROM files WHERE path = ?1");
            for (std::size_t i = 0; i < to_analyze.size(); ++i) {
                const FileEntry &e = results[i];
                if (!e.error.empty()) {
                    ++report.errors;
                }
                upsert.reset();
                upsert.bind(1, e.path)
                    .bind(2, e.sha256)
                    .bind(3, to_analyze[i]->size)
                    .bind(4, to_analyze[i]->mtime)
                    .bind(5, e.duration_s)
                    .bind(6, e.sample_rate)
                    .bind(7, e.channels)
                    .bind(8, e.error)
                    .bind(9, file_entry_to_json_string(e, false));
                upsert.step();
                id_of.reset();
                id_of.bind(1, e.path);
                if (id_of.step()) {
                    refresh_fts(id_of.i64(0));
                }
            }
            // 6. Missing rows.
            Stmt miss(db_, "UPDATE files SET present = 0 WHERE id = ?1");
            for (const auto &[path, k] : known) {
                if (!on_disk.count(path) && !claimed.count(k.id)) {
                    miss.reset();
                    miss.bind(1, k.id);
                    miss.step();
                    ++report.missing;
                }
            }
            set_meta("engine_version", engine);
            set_meta("mapping_version", mapping);
            set_meta("ref_spl", ref_spl);
            set_meta("updated_at", iso8601_utc_now());
            exec(db_, "COMMIT");
        } catch (...) {
            exec(db_, "ROLLBACK");
            throw;
        }
        if (options.prune) {
            report.pruned = prune();
            report.missing = 0;
        }
    } catch (const std::exception &e) {
        err = e.what();
    }
    return report;
}

std::size_t Library::prune() {
    std::vector<std::int64_t> ids;
    {
        Stmt s(db_, "SELECT id FROM files WHERE present = 0");
        while (s.step()) {
            ids.push_back(s.i64(0));
        }
    }
    exec(db_, "BEGIN");
    Stmt del_search(db_, "DELETE FROM search WHERE rowid = ?1");
    Stmt del_ann(db_, "DELETE FROM annotations WHERE file_id = ?1");
    Stmt del_file(db_, "DELETE FROM files WHERE id = ?1");
    for (std::int64_t id : ids) {
        del_search.reset();
        del_search.bind(1, id);
        del_search.step();
        del_ann.reset();
        del_ann.bind(1, id);
        del_ann.step();
        del_file.reset();
        del_file.bind(1, id);
        del_file.step();
    }
    exec(db_, "COMMIT");
    return ids.size();
}

std::optional<LibraryRow> Library::row_by_id(std::int64_t id) const {
    Stmt s(db_, std::string("SELECT ") + kRowColumns +
                    " FROM files f LEFT JOIN annotations a ON a.file_id = f.id WHERE f.id = ?1");
    s.bind(1, id);
    if (!s.step()) {
        return std::nullopt;
    }
    return row_from_stmt(s);
}

std::optional<LibraryRow> Library::get(std::string_view rel_path) const {
    Stmt s(db_, std::string("SELECT ") + kRowColumns +
                    " FROM files f LEFT JOIN annotations a ON a.file_id = f.id WHERE f.path = ?1");
    s.bind(1, rel_path);
    if (!s.step()) {
        return std::nullopt;
    }
    return row_from_stmt(s);
}

std::vector<LibraryRow> Library::search(const SearchQuery &q, std::string &err) const {
    std::vector<LibraryRow> out;
    try {
        std::string fts = fts_query_from_text(q.text);
        std::string sql = std::string("SELECT ") + kRowColumns + " FROM files f ";
        if (!fts.empty()) {
            sql += "JOIN search s ON s.rowid = f.id ";
        }
        sql += "LEFT JOIN annotations a ON a.file_id = f.id WHERE 1 = 1 ";
        std::vector<std::string> binds;
        if (!fts.empty()) {
            sql += "AND search MATCH ?" + std::to_string(binds.size() + 1) + " ";
            binds.push_back(fts);
        }
        if (!q.include_missing) {
            sql += "AND f.present = 1 ";
        }
        if (!q.cat_id_glob.empty()) {
            sql += "AND a.cat_id GLOB ?" + std::to_string(binds.size() + 1) + " ";
            binds.push_back(q.cat_id_glob);
        }
        if (!q.category.empty()) {
            std::vector<const UcsEntry *> in_cat = ucs_in_category(q.category);
            if (in_cat.empty()) {
                return out; // unknown category matches nothing
            }
            sql += "AND a.cat_id IN (";
            for (std::size_t k = 0; k < in_cat.size(); ++k) {
                if (k > 0) {
                    sql += ", ";
                }
                sql += "?" + std::to_string(binds.size() + 1);
                binds.push_back(std::string(in_cat[k]->cat_id));
            }
            sql += ") ";
        }
        if (q.unannotated) {
            sql += "AND (a.cat_id IS NULL OR a.cat_id = '') ";
        }
        std::optional<std::size_t> conf_bind;
        if (q.min_confidence >= 0.0) {
            conf_bind = binds.size() + 1;
            sql += "AND a.confidence >= ?" + std::to_string(*conf_bind) + " ";
            binds.push_back(""); // placeholder, bound as double below
        }
        sql += fts.empty() ? "ORDER BY f.path " : "ORDER BY bm25(search), f.path ";
        if (q.limit > 0) {
            sql += "LIMIT " + std::to_string(q.limit);
        }
        Stmt s(db_, sql);
        for (std::size_t k = 0; k < binds.size(); ++k) {
            if (conf_bind && k + 1 == *conf_bind) {
                s.bind(static_cast<int>(k + 1), q.min_confidence);
            } else {
                s.bind(static_cast<int>(k + 1), binds[k]);
            }
        }
        while (s.step()) {
            out.push_back(row_from_stmt(s));
        }
    } catch (const std::exception &e) {
        err = e.what();
    }
    return out;
}

std::size_t Library::count_files(bool present_only) const {
    Stmt s(db_, present_only ? "SELECT COUNT(*) FROM files WHERE present = 1"
                             : "SELECT COUNT(*) FROM files");
    return s.step() ? static_cast<std::size_t>(s.i64(0)) : 0;
}

std::size_t Library::count_annotated() const {
    Stmt s(db_, "SELECT COUNT(*) FROM files f JOIN annotations a ON a.file_id = f.id "
                "WHERE f.present = 1 AND a.cat_id != ''");
    return s.step() ? static_cast<std::size_t>(s.i64(0)) : 0;
}

std::vector<std::pair<std::string, std::size_t>> Library::category_counts() const {
    std::unordered_map<std::string, std::size_t> counts;
    Stmt s(db_, "SELECT a.cat_id, COUNT(*) FROM files f JOIN annotations a ON a.file_id = f.id "
                "WHERE f.present = 1 AND a.cat_id != '' GROUP BY a.cat_id");
    while (s.step()) {
        if (const UcsEntry *e = ucs_find(s.text(0))) {
            counts[std::string(e->category)] += static_cast<std::size_t>(s.i64(1));
        }
    }
    std::vector<std::pair<std::string, std::size_t>> out;
    for (std::string_view c : ucs_categories()) {
        auto it = counts.find(std::string(c));
        if (it != counts.end()) {
            out.emplace_back(it->first, it->second);
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::size_t>>
Library::cat_id_counts(std::string_view category) const {
    std::unordered_map<std::string, std::size_t> counts;
    Stmt s(db_, "SELECT a.cat_id, COUNT(*) FROM files f JOIN annotations a ON a.file_id = f.id "
                "WHERE f.present = 1 AND a.cat_id != '' GROUP BY a.cat_id");
    while (s.step()) {
        counts[s.text(0)] = static_cast<std::size_t>(s.i64(1));
    }
    std::vector<std::pair<std::string, std::size_t>> out;
    for (const UcsEntry *e : ucs_in_category(category)) {
        auto it = counts.find(std::string(e->cat_id));
        if (it != counts.end()) {
            out.emplace_back(it->first, it->second);
        }
    }
    return out;
}

SetResult Library::set_annotation(std::string_view rel_path, const Annotation &annotation_in,
                                  bool force) {
    std::optional<LibraryRow> row = get(rel_path);
    if (!row) {
        return SetResult::kUnknownPath;
    }
    Annotation a = annotation_in;
    if (a.source == AnnotationSource::kHuman) {
        a.locked = true;
    }
    if (row->annotation) {
        const Annotation &old = *row->annotation;
        // A lock is only ever released by a human write; `force` cannot bypass it (§6.2).
        if (old.locked && a.source != AnnotationSource::kHuman) {
            return SetResult::kSkippedLocked;
        }
        if (!force && static_cast<int>(a.source) < static_cast<int>(old.source)) {
            return SetResult::kSkippedLowerPrecedence;
        }
    }
    if (a.annotated_at.empty()) {
        a.annotated_at = iso8601_utc_now();
    }
    Stmt s(db_, "INSERT INTO annotations(file_id, cat_id, fx_name, description, keywords, "
                "confidence, source, model, prompt_version, annotated_at, locked, candidates_json) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12) "
                "ON CONFLICT(file_id) DO UPDATE SET cat_id = excluded.cat_id, fx_name = "
                "excluded.fx_name, description = excluded.description, keywords = "
                "excluded.keywords, confidence = excluded.confidence, source = excluded.source, "
                "model = excluded.model, prompt_version = excluded.prompt_version, annotated_at = "
                "excluded.annotated_at, locked = excluded.locked, candidates_json = "
                "excluded.candidates_json");
    s.bind(1, row->id)
        .bind(2, a.cat_id)
        .bind(3, a.fx_name)
        .bind(4, a.description)
        .bind(5, keywords_to_json(a.keywords))
        .bind(6, a.confidence)
        .bind(7, std::string_view(annotation_source_name(a.source)))
        .bind(8, a.model)
        .bind(9, a.prompt_version)
        .bind(10, a.annotated_at)
        .bind(11, a.locked ? 1 : 0)
        .bind(12, a.candidates_json);
    s.step();
    refresh_fts(row->id);
    return SetResult::kWritten;
}

bool Library::clear_annotation(std::string_view rel_path) {
    std::optional<LibraryRow> row = get(rel_path);
    if (!row) {
        return false;
    }
    Stmt s(db_, "DELETE FROM annotations WHERE file_id = ?1");
    s.bind(1, row->id);
    s.step();
    refresh_fts(row->id);
    return true;
}

bool Library::set_locked(std::string_view rel_path, bool locked) {
    std::optional<LibraryRow> row = get(rel_path);
    if (!row || !row->annotation) {
        return false;
    }
    Stmt s(db_, "UPDATE annotations SET locked = ?2 WHERE file_id = ?1");
    s.bind(1, row->id).bind(2, locked ? 1 : 0);
    s.step();
    return true;
}

ClassifyReport Library::classify_offline(bool force) {
    ClassifyReport report;
    std::vector<std::string> paths;
    {
        Stmt s(db_, "SELECT path FROM files WHERE present = 1 ORDER BY path");
        while (s.step()) {
            paths.push_back(s.text(0));
        }
    }
    exec(db_, "BEGIN");
    for (const std::string &p : paths) {
        ++report.examined;
        std::optional<Annotation> a = classify_offline_path(p);
        if (!a) {
            ++report.unmatched;
            continue;
        }
        switch (set_annotation(p, *a, force)) {
        case SetResult::kWritten:
            ++report.written;
            break;
        case SetResult::kSkippedLocked:
            ++report.skipped_locked;
            break;
        case SetResult::kSkippedLowerPrecedence:
            ++report.skipped_precedence;
            break;
        case SetResult::kUnknownPath:
            break;
        }
    }
    exec(db_, "COMMIT");
    return report;
}

Manifest Library::export_manifest(const SearchQuery *query, const std::string &root_as_given,
                                  std::string &err) const {
    Manifest m;
    m.root = root_as_given;
    m.engine_version = kVersionString;
    m.ref_spl = active_mapping_config().ref_spl;
    m.mapping_version = active_mapping_config().mapping_version;
    std::vector<LibraryRow> rows;
    if (query) {
        SearchQuery q = *query;
        q.limit = 0;
        rows = search(q, err);
    } else {
        SearchQuery q;
        q.limit = 0;
        rows = search(q, err);
    }
    for (LibraryRow &r : rows) {
        m.files.push_back(std::move(r.entry));
    }
    std::sort(m.files.begin(), m.files.end(),
              [](const FileEntry &a, const FileEntry &b) { return a.path < b.path; });
    recompute_stats(m);
    return m;
}

} // namespace sp
