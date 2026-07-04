#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
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

int cmd_not_implemented(const char *name) {
    std::fprintf(stderr, "soundpalette %s: not implemented yet\n", name);
    return 2;
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
            return cmd_not_implemented("watch");
        } else {
            std::fprintf(stderr, "soundpalette: unknown subcommand '%s'\n", subcommand.c_str());
            return 2;
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "soundpalette: error: %s\n", e.what());
        return 2;
    }
}
