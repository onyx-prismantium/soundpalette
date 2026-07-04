#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "soundpalette/glyph.h"
#include "soundpalette/lint.h"
#include "soundpalette/manifest.h"
#include "soundpalette/mapping.h"
#include "soundpalette/version.h"

namespace {

std::vector<std::string> to_vec(int argc, char **argv, int start) {
    std::vector<std::string> v;
    for (int i = start; i < argc; ++i) {
        v.emplace_back(argv[i]);
    }
    return v;
}

bool write_or_print(const std::string &out_path, const std::string &content) {
    if (out_path.empty()) {
        std::fwrite(content.data(), 1, content.size(), stdout);
        std::fputc('\n', stdout);
        return true;
    }
    std::ofstream f(out_path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "soundpalette: cannot write %s\n", out_path.c_str());
        return false;
    }
    f << content << "\n";
    return true;
}

int cmd_scan(const std::vector<std::string> &args) {
    std::string dir;
    std::string out;
    bool no_meta = false;
    bool quiet = false;
    int threads = 0;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--out" && i + 1 < args.size()) {
            out = args[++i];
        } else if (a == "--no-meta") {
            no_meta = true;
        } else if (a == "--quiet") {
            quiet = true;
        } else if (a == "--threads" && i + 1 < args.size()) {
            threads = std::stoi(args[++i]);
        } else if (a.rfind("--", 0) != 0 && dir.empty()) {
            dir = a;
        }
    }

    if (dir.empty()) {
        std::fprintf(stderr, "usage: soundpalette scan <dir> [--out palette.json] [--no-meta]\n");
        return 2;
    }
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        std::fprintf(stderr, "soundpalette: not a directory: %s\n", dir.c_str());
        return 2;
    }

    sp::ScanOptions options;
    options.threads = threads;
    options.include_meta = !no_meta;

    auto t0 = std::chrono::steady_clock::now();
    sp::Manifest manifest = sp::scan_directory(dir, options);
    auto t1 = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(t1 - t0).count();

    std::string json = sp::manifest_to_json(manifest);
    if (!write_or_print(out, json)) {
        return 2;
    }

    if (!quiet) {
        std::fprintf(stderr, "scanned %zu files in %.3f s\n", manifest.files.size(), seconds);
    }
    return 0;
}

int cmd_print_mapping(const std::vector<std::string> &args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--out" && i + 1 < args.size()) {
            out = args[++i];
        }
    }
    std::string json = sp::mapping_config_to_json(sp::active_mapping_config());
    return write_or_print(out, json) ? 0 : 2;
}

bool load_baseline_stats(const std::string &path, sp::Manifest &baseline, std::string &err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ss.str());
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
    if (!j.contains("stats")) {
        err = path + ": missing 'stats' block";
        return false;
    }

    static const char *kDimNames[7] = {"bright01", "warm01", "ton01",   "atk01",
                                       "tail01",   "loud01", "jitter01"};
    for (int d = 0; d < 7; ++d) {
        if (!j["stats"].contains(kDimNames[d])) {
            err = path + ": stats missing '" + kDimNames[d] + "'";
            return false;
        }
        const auto &s = j["stats"][kDimNames[d]];
        baseline.stats[static_cast<std::size_t>(d)].mean = s.at("mean").get<double>();
        baseline.stats[static_cast<std::size_t>(d)].std = s.at("std").get<double>();
        baseline.stats[static_cast<std::size_t>(d)].min = s.at("min").get<double>();
        baseline.stats[static_cast<std::size_t>(d)].max = s.at("max").get<double>();
    }
    return true;
}

int cmd_lint(const std::vector<std::string> &args) {
    std::string dir;
    std::string baseline_path;
    double threshold = 2.5;
    int top = 10;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--baseline" && i + 1 < args.size()) {
            baseline_path = args[++i];
        } else if (a == "--threshold" && i + 1 < args.size()) {
            threshold = std::stod(args[++i]);
        } else if (a == "--top" && i + 1 < args.size()) {
            top = std::stoi(args[++i]);
        } else if (a.rfind("--", 0) != 0 && dir.empty()) {
            dir = a;
        }
    }

    if (dir.empty() || baseline_path.empty()) {
        std::fprintf(stderr, "usage: soundpalette lint <dir> --baseline palette.json "
                             "[--threshold 2.5] [--top 10]\n");
        return 2;
    }
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        std::fprintf(stderr, "soundpalette: not a directory: %s\n", dir.c_str());
        return 2;
    }

    sp::Manifest baseline;
    std::string err;
    if (!load_baseline_stats(baseline_path, baseline, err)) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }

    sp::Manifest candidate = sp::scan_directory(dir, sp::ScanOptions{});
    sp::LintReport report = sp::lint(baseline, candidate, threshold);

    if (report.outliers.empty()) {
        std::fprintf(stdout, "PASS %d files within palette\n", report.considered_files);
        return 0;
    }

    int shown = 0;
    for (const sp::LintFileResult &r : report.outliers) {
        if (shown >= top) {
            break;
        }
        std::string dims;
        for (std::size_t i = 0; i < r.offending_dims.size(); ++i) {
            if (i > 0) {
                dims += ",";
            }
            dims += r.offending_dims[i];
        }
        std::fprintf(stdout, "OUTLIER %s worst=%s z=%+.2f dims=%s\n", r.path.c_str(),
                     r.worst_dim.c_str(), r.worst_z, dims.c_str());
        ++shown;
    }
    return 1;
}

int cmd_export_svg(const std::vector<std::string> &args) {
    std::string manifest_path;
    std::string out;
    int columns = 8;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--out" && i + 1 < args.size()) {
            out = args[++i];
        } else if (a == "--columns" && i + 1 < args.size()) {
            columns = std::stoi(args[++i]);
        } else if (a.rfind("--", 0) != 0 && manifest_path.empty()) {
            manifest_path = a;
        }
    }

    if (manifest_path.empty() || out.empty()) {
        std::fprintf(stderr, "usage: soundpalette export-svg <palette.json> --out sheet.svg "
                             "[--columns 8]\n");
        return 2;
    }

    std::ifstream f(manifest_path);
    if (!f) {
        std::fprintf(stderr, "soundpalette: cannot open %s\n", manifest_path.c_str());
        return 2;
    }
    std::ostringstream ss;
    ss << f.rdbuf();

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ss.str());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "soundpalette: %s: %s\n", manifest_path.c_str(), e.what());
        return 2;
    }

    sp::Manifest manifest;
    manifest.schema_version = j.value("schema_version", 1);
    manifest.mapping_version = j.value("mapping_version", 1);
    manifest.root = j.value("root", "");
    for (const auto &fj : j.at("files")) {
        sp::FileEntry fe;
        fe.path = fj.value("path", "");
        fe.error = fj.value("error", "");
        if (fe.error.empty() && fj.contains("visual")) {
            const auto &vj = fj["visual"];
            fe.visual.hue_deg = vj.value("hue_deg", 0.0);
            fe.visual.sat = vj.value("sat", 0.0);
            fe.visual.light = vj.value("light", 0.0);
            fe.visual.size_px = vj.value("size_px", 0.0);
            fe.visual.spike01 = vj.value("spike01", 0.0);
            fe.visual.spikes = vj.value("spikes", 0);
            fe.visual.jitter01 = vj.value("jitter01", 0.0);
            fe.visual.tail01 = vj.value("tail01", 0.0);
            fe.visual.seed = vj.value("seed", static_cast<std::uint64_t>(0));
        }
        manifest.files.push_back(std::move(fe));
    }

    std::string svg = sp::sheet_svg(manifest, columns);
    std::ofstream out_f(out, std::ios::binary);
    if (!out_f) {
        std::fprintf(stderr, "soundpalette: cannot write %s\n", out.c_str());
        return 2;
    }
    out_f << svg;
    return 0;
}

volatile std::sig_atomic_t g_watch_stop = 0;

void watch_signal_handler(int) {
    g_watch_stop = 1;
}

// Atomic manifest rewrite (§9 watch): write to a temp file next to the target, then rename.
bool write_atomic(const std::string &out_path, const std::string &content) {
    std::string tmp_path = out_path + ".tmp";
    {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::fprintf(stderr, "soundpalette: cannot write %s\n", tmp_path.c_str());
            return false;
        }
        f << content << "\n";
        if (!f.flush()) {
            std::fprintf(stderr, "soundpalette: write failed for %s\n", tmp_path.c_str());
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, out_path, ec);
    if (ec) {
        std::fprintf(stderr, "soundpalette: rename %s -> %s failed: %s\n", tmp_path.c_str(),
                     out_path.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

// Relative forward-slash path -> (absolute path, mtime) for every supported audio file under
// root. The polling watcher diffs consecutive snapshots (mtime + file set, §9).
std::map<std::string, std::pair<std::filesystem::path, std::filesystem::file_time_type>>
watch_snapshot(const std::filesystem::path &root) {
    std::map<std::string, std::pair<std::filesystem::path, std::filesystem::file_time_type>> snap;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec) || !sp::has_supported_audio_extension(it->path())) {
            continue;
        }
        std::filesystem::file_time_type mtime = std::filesystem::last_write_time(it->path(), ec);
        if (ec) {
            ec.clear();
            continue; // file vanished mid-poll; the next interval picks it up
        }
        std::string rel = std::filesystem::relative(it->path(), root).generic_string();
        snap.emplace(std::move(rel), std::make_pair(it->path(), mtime));
    }
    return snap;
}

int cmd_watch(const std::vector<std::string> &args) {
    std::string dir;
    std::string out = "palette.json";
    int interval_ms = 500;
    bool quiet = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--out" && i + 1 < args.size()) {
            out = args[++i];
        } else if (a == "--interval-ms" && i + 1 < args.size()) {
            interval_ms = std::stoi(args[++i]);
        } else if (a == "--quiet") {
            quiet = true;
        } else if (a.rfind("--", 0) != 0 && dir.empty()) {
            dir = a;
        }
    }

    if (dir.empty()) {
        std::fprintf(stderr,
                     "usage: soundpalette watch <dir> [--out palette.json] [--interval-ms 500]\n");
        return 2;
    }
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        std::fprintf(stderr, "soundpalette: not a directory: %s\n", dir.c_str());
        return 2;
    }
    interval_ms = std::max(interval_ms, 10);

    std::signal(SIGINT, watch_signal_handler);
    std::signal(SIGTERM, watch_signal_handler);

    sp::Manifest manifest = sp::scan_directory(dir, sp::ScanOptions{});
    if (!write_atomic(out, sp::manifest_to_json(manifest))) {
        return 2;
    }
    if (!quiet) {
        std::fprintf(stderr, "watch: initial scan, %zu files -> %s\n", manifest.files.size(),
                     out.c_str());
    }

    auto prev = watch_snapshot(dir);

    while (!g_watch_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        if (g_watch_stop) {
            break;
        }

        auto cur = watch_snapshot(dir);

        std::vector<std::string> removed;
        std::vector<std::string> changed; // added or mtime-changed: re-analyze these only
        for (const auto &[rel, info] : prev) {
            if (cur.find(rel) == cur.end()) {
                removed.push_back(rel);
            }
        }
        for (const auto &[rel, info] : cur) {
            auto it = prev.find(rel);
            if (it == prev.end() || it->second.second != info.second) {
                changed.push_back(rel);
            }
        }

        if (removed.empty() && changed.empty()) {
            prev = std::move(cur);
            continue;
        }

        for (const std::string &rel : removed) {
            auto it = std::find_if(manifest.files.begin(), manifest.files.end(),
                                   [&](const sp::FileEntry &e) { return e.path == rel; });
            if (it != manifest.files.end()) {
                manifest.files.erase(it);
            }
        }
        for (const std::string &rel : changed) {
            sp::FileEntry entry = sp::analyze_file(dir, cur.at(rel).first);
            auto it = std::find_if(manifest.files.begin(), manifest.files.end(),
                                   [&](const sp::FileEntry &e) { return e.path == entry.path; });
            if (it != manifest.files.end()) {
                *it = std::move(entry);
            } else {
                manifest.files.push_back(std::move(entry));
            }
        }
        std::sort(manifest.files.begin(), manifest.files.end(),
                  [](const sp::FileEntry &a, const sp::FileEntry &b) { return a.path < b.path; });
        sp::recompute_stats(manifest);

        if (!write_atomic(out, sp::manifest_to_json(manifest))) {
            return 2;
        }
        if (!quiet) {
            std::fprintf(stderr, "watch: %zu changed, %zu removed -> %s (%zu files)\n",
                         changed.size(), removed.size(), out.c_str(), manifest.files.size());
        }
        prev = std::move(cur);
    }

    if (!quiet) {
        std::fprintf(stderr, "watch: stopped\n");
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: soundpalette <scan|lint|export-svg|watch|print-mapping> [args...]\n");
        return 2;
    }

    std::string subcommand = argv[1];
    std::vector<std::string> args = to_vec(argc, argv, 2);

    try {
        if (subcommand == "scan") {
            return cmd_scan(args);
        } else if (subcommand == "print-mapping") {
            return cmd_print_mapping(args);
        } else if (subcommand == "lint") {
            return cmd_lint(args);
        } else if (subcommand == "export-svg") {
            return cmd_export_svg(args);
        } else if (subcommand == "watch") {
            return cmd_watch(args);
        } else {
            std::fprintf(stderr, "soundpalette: unknown subcommand '%s'\n", subcommand.c_str());
            return 2;
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "soundpalette: error: %s\n", e.what());
        return 2;
    }
}
