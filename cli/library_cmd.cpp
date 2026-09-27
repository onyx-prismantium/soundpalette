// `soundpalette library ...` (extension-4 §9.1). Every verb locates the index by walking up
// from the given path, so a sub-folder or a single file works as the argument.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "soundpalette/library.h"
#include "soundpalette/manifest.h"
#include "soundpalette/ucs.h"

namespace {

using json = nlohmann::ordered_json;

int usage() {
    std::fprintf(
        stderr,
        "usage: soundpalette library <verb> ...\n"
        "  init <dir> [--threads n] [--quiet]            create the index, ingest, classify "
        "offline\n"
        "  update <path> [--prune] [--threads n] [--quiet] [--json]\n"
        "  classify <path> [--force] [--json]            offline pass (UCS filenames, tokens)\n"
        "  search <path> [words...] [--catid GLOB] [--category NAME] [--unannotated]\n"
        "         [--min-confidence x] [--limit n] [--json]\n"
        "  show <file> [--json]\n"
        "  stats <path> [--json]\n"
        "  export <path> --manifest out.json [--query \"...\"] [--catid GLOB] [--category NAME]\n"
        "         [--unannotated]\n"
        "  set <file> [--catid X] [--fx-name N] [--description D] [--keywords a,b] [--unlock]\n"
        "                                                human edit (locks the row)\n");
    return 2;
}

std::unique_ptr<sp::Library> open_for(const std::string &arg, std::string &err) {
    std::optional<std::filesystem::path> root = sp::Library::find_root(arg);
    if (!root) {
        err = "no library found at or above " + arg + " (run: soundpalette library init <dir>)";
        return nullptr;
    }
    return sp::Library::open(*root, err);
}

std::string rel_of(const sp::Library &lib, const std::string &arg) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(arg, ec).lexically_normal();
    return std::filesystem::relative(abs, lib.root(), ec).generic_string();
}

json annotation_json(const sp::Annotation &a) {
    json j = json::object();
    j["cat_id"] = a.cat_id;
    if (const sp::UcsEntry *e = sp::ucs_find(a.cat_id)) {
        j["category"] = std::string(e->category);
        j["sub_category"] = std::string(e->sub_category);
    } else {
        j["category"] = "";
        j["sub_category"] = "";
    }
    j["fx_name"] = a.fx_name;
    j["description"] = a.description;
    j["keywords"] = a.keywords;
    j["confidence"] = a.confidence;
    j["source"] = sp::annotation_source_name(a.source);
    j["model"] = a.model;
    j["prompt_version"] = a.prompt_version;
    j["annotated_at"] = a.annotated_at;
    j["locked"] = a.locked;
    return j;
}

json row_json(const sp::LibraryRow &r, bool with_entry) {
    json j = json::object();
    j["path"] = r.path;
    j["present"] = r.present;
    j["duration_s"] = r.entry.duration_s;
    j["error"] = r.entry.error;
    if (r.annotation) {
        j["annotation"] = annotation_json(*r.annotation);
    } else {
        j["annotation"] = nullptr;
    }
    if (with_entry) {
        std::string err;
        j["entry"] = nlohmann::ordered_json::parse(sp::file_entry_to_json_string(r.entry, true));
    }
    return j;
}

json update_json(const sp::UpdateReport &r) {
    json j = json::object();
    j["added"] = r.added;
    j["changed"] = r.changed;
    j["moved"] = r.moved;
    j["unchanged"] = r.unchanged;
    j["returned"] = r.returned;
    j["missing"] = r.missing;
    j["pruned"] = r.pruned;
    j["analyzed"] = r.analyzed;
    j["errors"] = r.errors;
    j["reanalyzed_all"] = r.reanalyzed_all;
    return j;
}

json classify_json(const sp::ClassifyReport &r) {
    json j = json::object();
    j["examined"] = r.examined;
    j["written"] = r.written;
    j["skipped_locked"] = r.skipped_locked;
    j["skipped_precedence"] = r.skipped_precedence;
    j["unmatched"] = r.unmatched;
    return j;
}

void print_update(const sp::UpdateReport &r) {
    std::printf("library update: %zu added, %zu changed, %zu moved, %zu unchanged, %zu missing"
                "%s (%zu analyzed, %zu errors)%s\n",
                r.added, r.changed, r.moved, r.unchanged, r.missing,
                r.pruned ? (", " + std::to_string(r.pruned) + " pruned").c_str() : "", r.analyzed,
                r.errors, r.reanalyzed_all ? " [engine changed: full re-analysis]" : "");
}

void print_classify(const sp::ClassifyReport &r) {
    std::printf("library classify: %zu examined, %zu written, %zu locked, %zu kept (higher "
                "source), %zu unmatched\n",
                r.examined, r.written, r.skipped_locked, r.skipped_precedence, r.unmatched);
}

bool parse_query_flags(const std::vector<std::string> &args, std::size_t start, sp::SearchQuery &q,
                       bool &as_json, std::vector<std::string> &positionals,
                       std::string &out_path) {
    for (std::size_t i = start; i < args.size(); ++i) {
        const std::string &a = args[i];
        auto next = [&](std::string &dst) {
            if (i + 1 >= args.size()) {
                return false;
            }
            dst = args[++i];
            return true;
        };
        std::string v;
        if (a == "--json") {
            as_json = true;
        } else if (a == "--unannotated") {
            q.unannotated = true;
        } else if (a == "--include-missing") {
            q.include_missing = true;
        } else if (a == "--catid") {
            if (!next(q.cat_id_glob)) {
                return false;
            }
        } else if (a == "--category") {
            if (!next(q.category)) {
                return false;
            }
        } else if (a == "--query") {
            if (!next(q.text)) {
                return false;
            }
        } else if (a == "--manifest") {
            if (!next(out_path)) {
                return false;
            }
        } else if (a == "--min-confidence") {
            if (!next(v)) {
                return false;
            }
            q.min_confidence = std::stod(v);
        } else if (a == "--limit") {
            if (!next(v)) {
                return false;
            }
            q.limit = std::stoi(v);
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "soundpalette: unknown option %s\n", a.c_str());
            return false;
        } else {
            positionals.push_back(a);
        }
    }
    return true;
}

int cmd_init_or_update(const std::vector<std::string> &args, bool is_init) {
    if (args.size() < 2) {
        return usage();
    }
    sp::UpdateOptions opt;
    bool quiet = false;
    bool as_json = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--threads" && i + 1 < args.size()) {
            opt.threads = std::stoi(args[++i]);
        } else if (args[i] == "--quiet") {
            quiet = true;
        } else if (args[i] == "--prune") {
            opt.prune = true;
        } else if (args[i] == "--json") {
            as_json = true;
        } else {
            return usage();
        }
    }
    std::string err;
    std::unique_ptr<sp::Library> lib =
        is_init ? sp::Library::create(args[1], err) : open_for(args[1], err);
    if (!lib) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }
    if (!quiet && !as_json) {
        opt.on_progress = [](std::size_t done, std::size_t total) {
            std::fprintf(stderr, "\ranalyzed %zu/%zu", done, total);
            if (done == total) {
                std::fputc('\n', stderr);
            }
        };
    }
    sp::UpdateReport rep = lib->update(opt, err);
    if (!err.empty()) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }
    sp::ClassifyReport crep;
    if (is_init) {
        crep = lib->classify_offline(false);
    }
    if (as_json) {
        json j = json::object();
        j["root"] = lib->root().generic_string();
        j["update"] = update_json(rep);
        if (is_init) {
            j["classify"] = classify_json(crep);
        }
        std::printf("%s\n", j.dump(2).c_str());
    } else {
        if (is_init) {
            std::printf("library created at %s\n",
                        sp::Library::index_path(lib->root()).generic_string().c_str());
        }
        print_update(rep);
        if (is_init) {
            print_classify(crep);
        }
    }
    return rep.errors > 0 && rep.errors == rep.analyzed && rep.analyzed > 0 ? 1 : 0;
}

int cmd_classify(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        return usage();
    }
    bool force = false;
    bool as_json = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--force") {
            force = true;
        } else if (args[i] == "--json") {
            as_json = true;
        } else if (args[i] == "--offline") {
            // the only mode until M16; accepted for forward compatibility
        } else {
            return usage();
        }
    }
    std::string err;
    std::unique_ptr<sp::Library> lib = open_for(args[1], err);
    if (!lib) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }
    sp::ClassifyReport rep = lib->classify_offline(force);
    if (as_json) {
        std::printf("%s\n", classify_json(rep).dump(2).c_str());
    } else {
        print_classify(rep);
    }
    return 0;
}

void print_row_line(const sp::LibraryRow &r) {
    const char *cat = r.annotation ? r.annotation->cat_id.c_str() : "";
    std::string src = r.annotation ? sp::annotation_source_name(r.annotation->source) : "-";
    double conf = r.annotation ? r.annotation->confidence : 0.0;
    std::printf("%-10s %4.2f %-8s %s", cat[0] ? cat : "-", conf, src.c_str(), r.path.c_str());
    if (r.annotation && (!r.annotation->fx_name.empty() || !r.annotation->description.empty())) {
        std::printf("  -- %s", r.annotation->fx_name.c_str());
        if (!r.annotation->description.empty()) {
            std::printf(": %s", r.annotation->description.c_str());
        }
    }
    if (r.annotation && r.annotation->locked) {
        std::printf(" [locked]");
    }
    std::printf("\n");
}

int cmd_search(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        return usage();
    }
    sp::SearchQuery q;
    bool as_json = false;
    std::vector<std::string> words;
    std::string unused;
    if (!parse_query_flags(args, 2, q, as_json, words, unused)) {
        return usage();
    }
    for (const std::string &w : words) {
        if (!q.text.empty()) {
            q.text += ' ';
        }
        q.text += w;
    }
    std::string err;
    std::unique_ptr<sp::Library> lib = open_for(args[1], err);
    if (!lib) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }
    std::vector<sp::LibraryRow> rows = lib->search(q, err);
    if (!err.empty()) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }
    if (as_json) {
        json j = json::object();
        j["root"] = lib->root().generic_string();
        j["count"] = rows.size();
        json arr = json::array();
        for (const sp::LibraryRow &r : rows) {
            arr.push_back(row_json(r, false));
        }
        j["results"] = std::move(arr);
        std::printf("%s\n", j.dump(2).c_str());
    } else {
        for (const sp::LibraryRow &r : rows) {
            print_row_line(r);
        }
        std::printf("%zu result%s\n", rows.size(), rows.size() == 1 ? "" : "s");
    }
    return 0;
}

int cmd_show(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        return usage();
    }
    bool as_json = args.size() > 2 && args[2] == "--json";
    std::string err;
    std::unique_ptr<sp::Library> lib = open_for(args[1], err);
    if (!lib) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }
    std::string rel = rel_of(*lib, args[1]);
    std::optional<sp::LibraryRow> row = lib->get(rel);
    if (!row) {
        std::fprintf(stderr, "soundpalette: %s is not in the library (run library update)\n",
                     rel.c_str());
        return 1;
    }
    if (as_json) {
        std::printf("%s\n", row_json(*row, true).dump(2).c_str());
        return 0;
    }
    std::printf("path        %s%s\n", row->path.c_str(), row->present ? "" : " (missing)");
    std::printf("sha256      %s\n", row->sha256.c_str());
    std::printf("duration    %.3f s, %d Hz, %d ch%s\n", row->entry.duration_s,
                row->entry.sample_rate, row->entry.channels,
                row->entry.error.empty() ? "" : (" ERROR: " + row->entry.error).c_str());
    if (!row->annotation) {
        std::printf("annotation  (none)\n");
        return 0;
    }
    const sp::Annotation &a = *row->annotation;
    const sp::UcsEntry *e = sp::ucs_find(a.cat_id);
    std::printf(
        "cat_id      %s%s\n", a.cat_id.empty() ? "-" : a.cat_id.c_str(),
        e ? ("  (" + std::string(e->category) + " / " + std::string(e->sub_category) + ")").c_str()
          : "");
    std::printf("fx_name     %s\n", a.fx_name.c_str());
    std::printf("description %s\n", a.description.c_str());
    std::string kw;
    for (const std::string &k : a.keywords) {
        kw += (kw.empty() ? "" : ", ") + k;
    }
    std::printf("keywords    %s\n", kw.c_str());
    std::printf("confidence  %.2f\n", a.confidence);
    std::printf("source      %s%s%s%s\n", sp::annotation_source_name(a.source),
                a.model.empty() ? "" : (" model=" + a.model).c_str(),
                a.prompt_version.empty() ? "" : (" prompt=" + a.prompt_version).c_str(),
                a.locked ? " [locked]" : "");
    std::printf("annotated   %s\n", a.annotated_at.c_str());
    if (!a.candidates_json.empty()) {
        std::printf("candidates  %s\n", a.candidates_json.c_str());
    }
    return 0;
}

int cmd_stats(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        return usage();
    }
    bool as_json = args.size() > 2 && args[2] == "--json";
    std::string err;
    std::unique_ptr<sp::Library> lib = open_for(args[1], err);
    if (!lib) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }
    auto cats = lib->category_counts();
    if (as_json) {
        json j = json::object();
        j["root"] = lib->root().generic_string();
        j["files"] = lib->count_files(true);
        j["annotated"] = lib->count_annotated();
        j["ucs_version"] = lib->meta("ucs_version");
        j["engine_version"] = lib->meta("engine_version");
        json c = json::array();
        for (auto &[name, n] : cats) {
            json e = json::object();
            e["category"] = name;
            e["count"] = n;
            c.push_back(std::move(e));
        }
        j["categories"] = std::move(c);
        std::printf("%s\n", j.dump(2).c_str());
        return 0;
    }
    std::printf("library %s\n", lib->root().generic_string().c_str());
    std::printf("files %zu, annotated %zu, UCS v%s, engine %s\n", lib->count_files(true),
                lib->count_annotated(), lib->meta("ucs_version").c_str(),
                lib->meta("engine_version").c_str());
    for (auto &[name, n] : cats) {
        std::printf("  %-16s %zu\n", name.c_str(), n);
    }
    return 0;
}

int cmd_export(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        return usage();
    }
    sp::SearchQuery q;
    q.limit = 0;
    bool as_json = false;
    std::vector<std::string> words;
    std::string out_path;
    if (!parse_query_flags(args, 2, q, as_json, words, out_path) || out_path.empty()) {
        return usage();
    }
    std::string err;
    std::unique_ptr<sp::Library> lib = open_for(args[1], err);
    if (!lib) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }
    bool filtered = !q.text.empty() || !q.cat_id_glob.empty() || !q.category.empty() ||
                    q.unannotated || q.min_confidence >= 0.0;
    sp::Manifest m = lib->export_manifest(filtered ? &q : nullptr, args[1], err);
    if (!err.empty()) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }
    std::ofstream f(out_path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "soundpalette: cannot write %s\n", out_path.c_str());
        return 2;
    }
    f << sp::manifest_to_json(m) << "\n";
    std::printf("wrote %s (%zu files)\n", out_path.c_str(), m.files.size());
    return 0;
}

int cmd_set(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        return usage();
    }
    std::string err;
    std::unique_ptr<sp::Library> lib = open_for(args[1], err);
    if (!lib) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }
    std::string rel = rel_of(*lib, args[1]);
    std::optional<sp::LibraryRow> row = lib->get(rel);
    if (!row) {
        std::fprintf(stderr, "soundpalette: %s is not in the library\n", rel.c_str());
        return 1;
    }
    sp::Annotation a = row->annotation.value_or(sp::Annotation{});
    bool unlock = false;
    bool any = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string &o = args[i];
        if (o == "--unlock") {
            unlock = true;
            continue;
        }
        if (i + 1 >= args.size()) {
            return usage();
        }
        const std::string &v = args[++i];
        any = true;
        if (o == "--catid") {
            if (!v.empty() && sp::ucs_find(v) == nullptr) {
                std::fprintf(stderr, "soundpalette: unknown CatID %s\n", v.c_str());
                return 2;
            }
            a.cat_id = v;
        } else if (o == "--fx-name") {
            a.fx_name = v;
        } else if (o == "--description") {
            a.description = v;
        } else if (o == "--keywords") {
            a.keywords.clear();
            std::size_t s = 0;
            while (s <= v.size()) {
                std::size_t c = v.find(',', s);
                std::string k = v.substr(s, c == std::string::npos ? c : c - s);
                while (!k.empty() && k.front() == ' ') {
                    k.erase(k.begin());
                }
                while (!k.empty() && k.back() == ' ') {
                    k.pop_back();
                }
                if (!k.empty()) {
                    a.keywords.push_back(k);
                }
                if (c == std::string::npos) {
                    break;
                }
                s = c + 1;
            }
        } else {
            return usage();
        }
    }
    if (unlock && !any) {
        if (!lib->set_locked(rel, false)) {
            std::fprintf(stderr, "soundpalette: nothing to unlock\n");
            return 1;
        }
        std::printf("unlocked %s\n", rel.c_str());
        return 0;
    }
    a.source = sp::AnnotationSource::kHuman;
    a.confidence = 1.0;
    a.model.clear();
    a.prompt_version.clear();
    a.annotated_at.clear();
    a.locked = !unlock;
    sp::SetResult r = lib->set_annotation(rel, a, true);
    if (r != sp::SetResult::kWritten) {
        std::fprintf(stderr, "soundpalette: write failed\n");
        return 1;
    }
    if (unlock) {
        lib->set_locked(rel, false);
    }
    std::printf("annotated %s (%s)%s\n", rel.c_str(), a.cat_id.empty() ? "-" : a.cat_id.c_str(),
                unlock ? "" : " [locked]");
    return 0;
}

int cmd_ucs_impl(const std::vector<std::string> &args) {
    auto usage_ucs = [] {
        std::fprintf(stderr,
                     "usage: soundpalette ucs categories | list <CATEGORY> | find <CatID> | "
                     "rank <words...> [--category C] [--top n] [--json]\n");
        return 2;
    };
    if (args.empty()) {
        return usage_ucs();
    }
    const std::string &verb = args[0];
    if (verb == "categories") {
        for (std::string_view c : sp::ucs_categories()) {
            std::printf("%s\n", std::string(c).c_str());
        }
        return 0;
    }
    if (verb == "list") {
        std::string cat = args.size() > 1 ? args[1] : "";
        for (const sp::UcsEntry &e : sp::ucs_entries()) {
            if (cat.empty() || e.category == cat) {
                std::printf("%-10s %-16s %s\n", std::string(e.cat_id).c_str(),
                            std::string(e.sub_category).c_str(),
                            std::string(e.explanation).c_str());
            }
        }
        return 0;
    }
    if (verb == "find") {
        if (args.size() < 2) {
            return usage_ucs();
        }
        const sp::UcsEntry *e = sp::ucs_find(args[1]);
        if (!e) {
            std::fprintf(stderr, "unknown CatID %s\n", args[1].c_str());
            return 1;
        }
        std::printf("%s  %s / %s\n%s\n", std::string(e->cat_id).c_str(),
                    std::string(e->category).c_str(), std::string(e->sub_category).c_str(),
                    std::string(e->explanation).c_str());
        std::string syn;
        for (std::string_view s : e->synonyms) {
            syn += (syn.empty() ? "" : ", ") + std::string(s);
        }
        std::printf("synonyms: %s\n", syn.c_str());
        return 0;
    }
    if (verb == "rank") {
        std::vector<std::string> words;
        std::string category;
        int top = 10;
        bool as_json = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--category" && i + 1 < args.size()) {
                category = args[++i];
            } else if (args[i] == "--top" && i + 1 < args.size()) {
                top = std::stoi(args[++i]);
            } else if (args[i] == "--json") {
                as_json = true;
            } else {
                words.push_back(args[i]);
            }
        }
        std::string text;
        for (const std::string &w : words) {
            text += w + " ";
        }
        std::vector<std::string> tokens = sp::ucs_tokenize(text);
        std::vector<sp::UcsMatch> ranked = sp::ucs_rank(tokens, category);
        if (as_json) {
            json j = json::array();
            for (std::size_t k = 0; k < ranked.size() && static_cast<int>(k) < top; ++k) {
                json e = json::object();
                e["cat_id"] = std::string(ranked[k].entry->cat_id);
                e["category"] = std::string(ranked[k].entry->category);
                e["sub_category"] = std::string(ranked[k].entry->sub_category);
                e["score"] = ranked[k].score;
                j.push_back(std::move(e));
            }
            std::printf("%s\n", j.dump(2).c_str());
            return 0;
        }
        std::string tk;
        for (const std::string &t : tokens) {
            tk += (tk.empty() ? "" : " ") + t;
        }
        std::printf("tokens: %s\n", tk.c_str());
        for (std::size_t k = 0; k < ranked.size() && static_cast<int>(k) < top; ++k) {
            std::printf("%3d  %-10s %s / %s\n", ranked[k].score,
                        std::string(ranked[k].entry->cat_id).c_str(),
                        std::string(ranked[k].entry->category).c_str(),
                        std::string(ranked[k].entry->sub_category).c_str());
        }
        return 0;
    }
    return usage_ucs();
}

} // namespace

int cmd_ucs(const std::vector<std::string> &args) {
    return cmd_ucs_impl(args);
}

int cmd_library(const std::vector<std::string> &args) {
    if (args.empty()) {
        return usage();
    }
    const std::string &verb = args[0];
    if (verb == "init") {
        return cmd_init_or_update(args, true);
    }
    if (verb == "update") {
        return cmd_init_or_update(args, false);
    }
    if (verb == "classify") {
        return cmd_classify(args);
    }
    if (verb == "search") {
        return cmd_search(args);
    }
    if (verb == "show") {
        return cmd_show(args);
    }
    if (verb == "stats") {
        return cmd_stats(args);
    }
    if (verb == "export") {
        return cmd_export(args);
    }
    if (verb == "set") {
        return cmd_set(args);
    }
    return usage();
}
