#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "soundpalette/manifest.h"
#include "soundpalette/mapping.h"
#include "soundpalette/version.h"

namespace {

std::vector<std::string> to_vec(int argc, char** argv, int start) {
    std::vector<std::string> v;
    for (int i = start; i < argc; ++i) {
        v.emplace_back(argv[i]);
    }
    return v;
}

bool write_or_print(const std::string& out_path, const std::string& content) {
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

int cmd_scan(const std::vector<std::string>& args) {
    std::string dir;
    std::string out;
    bool no_meta = false;
    bool quiet = false;
    int threads = 0;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
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

int cmd_print_mapping(const std::vector<std::string>& args) {
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--out" && i + 1 < args.size()) {
            out = args[++i];
        }
    }
    std::string json = sp::mapping_config_to_json(sp::active_mapping_config());
    return write_or_print(out, json) ? 0 : 2;
}

int cmd_not_implemented(const char* name) {
    std::fprintf(stderr, "soundpalette %s: not implemented yet\n", name);
    return 2;
}

} // namespace

int main(int argc, char** argv) {
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
            return cmd_not_implemented("lint");
        } else if (subcommand == "export-svg") {
            return cmd_not_implemented("export-svg");
        } else if (subcommand == "watch") {
            return cmd_not_implemented("watch");
        } else {
            std::fprintf(stderr, "soundpalette: unknown subcommand '%s'\n", subcommand.c_str());
            return 2;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "soundpalette: error: %s\n", e.what());
        return 2;
    }
}
