#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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

#include "soundpalette/capability.h"
#include "soundpalette/describe.h"
#include "soundpalette/deviation.h"
#include "soundpalette/glyph.h"
#include "soundpalette/lint.h"
#include "soundpalette/manifest.h"
#include "soundpalette/mapping.h"
#include "soundpalette/presets.h"
#include "soundpalette/profile.h"
#include "soundpalette/propose.h"
#include "soundpalette/psycho.h"
#include "soundpalette/recipe.h"
#include "soundpalette/version.h"

namespace {

// Rounds to 4 decimals for canonical JSON output (PLAN.md §8 rules).
double round4(double x) {
    return std::round(x * 10000.0) / 10000.0;
}

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
    bool no_psycho = false;
    bool quiet = false;
    int threads = 0;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--out" && i + 1 < args.size()) {
            out = args[++i];
        } else if (a == "--no-meta") {
            no_meta = true;
        } else if (a == "--no-psycho") {
            no_psycho = true;
        } else if (a == "--quiet") {
            quiet = true;
        } else if (a == "--threads" && i + 1 < args.size()) {
            threads = std::stoi(args[++i]);
        } else if (a.rfind("--", 0) != 0 && dir.empty()) {
            dir = a;
        }
    }

    if (dir.empty()) {
        std::fprintf(
            stderr,
            "usage: soundpalette scan <dir> [--out palette.json] [--no-meta] [--no-psycho]\n");
        return 2;
    }
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        std::fprintf(stderr, "soundpalette: not a directory: %s\n", dir.c_str());
        return 2;
    }

    sp::ScanOptions options;
    options.threads = threads;
    options.include_meta = !no_meta;
    options.with_psycho = !no_psycho;

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
    baseline.mapping_version = j.value("mapping_version", 1);
    baseline.ref_spl = j.value("ref_spl", 75.0);

    static const char *kDimNames[8] = {"bright01", "warm01", "ton01",    "atk01",
                                       "tail01",   "loud01", "jitter01", "fluct01"};
    for (int d = 0; d < 8; ++d) {
        if (!j["stats"].contains(kDimNames[d])) {
            err = path + ": stats missing '" + kDimNames[d] +
                  "' (mapping v1 baseline? rescan to regenerate a v2 manifest)";
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

// Loads a Profile from either --profile (.sppal.json) or --baseline (manifest json, adapted
// via profile_from_manifest so every code path consumes Profile only — extension-2 §4.3).
bool load_profile_arg(const std::string &baseline_path, const std::string &profile_path,
                      sp::Profile &out, std::string &err) {
    if (!profile_path.empty()) {
        std::ifstream f(profile_path);
        if (!f) {
            err = "cannot open " + profile_path;
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        auto p = sp::profile_from_json(ss.str(), err);
        if (!p.has_value()) {
            err = profile_path + ": " + err;
            return false;
        }
        out = std::move(*p);
        err = sp::profile_compat_error(out);
        if (!err.empty()) {
            err = profile_path + ": " + err;
            return false;
        }
        return true;
    }
    sp::Manifest baseline;
    if (!load_baseline_stats(baseline_path, baseline, err)) {
        return false;
    }
    out = sp::profile_from_manifest(baseline);
    err = sp::profile_compat_error(out);
    if (!err.empty()) {
        err = baseline_path + ": " + err + " (rescan the baseline folder)";
        return false;
    }
    return true;
}

int cmd_lint(const std::vector<std::string> &args) {
    std::string dir;
    std::string baseline_path;
    std::string profile_path;
    double threshold = -1.0; // <0 = use the profile's own threshold
    int top = 10;
    bool json_out = false;
    bool json_all = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--baseline" && i + 1 < args.size()) {
            baseline_path = args[++i];
        } else if (a == "--profile" && i + 1 < args.size()) {
            profile_path = args[++i];
        } else if (a == "--threshold" && i + 1 < args.size()) {
            threshold = std::stod(args[++i]);
        } else if (a == "--top" && i + 1 < args.size()) {
            top = std::stoi(args[++i]);
        } else if (a == "--json") {
            json_out = true;
        } else if (a == "--all") {
            json_all = true;
        } else if (a.rfind("--", 0) != 0 && dir.empty()) {
            dir = a;
        }
    }

    if (dir.empty() || (baseline_path.empty() && profile_path.empty())) {
        std::fprintf(stderr, "usage: soundpalette lint <dir> (--baseline m.json | --profile "
                             "p.sppal.json) [--threshold X] [--top N] [--json [--all]]\n");
        return 2;
    }
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        std::fprintf(stderr, "soundpalette: not a directory: %s\n", dir.c_str());
        return 2;
    }

    sp::Profile profile;
    std::string err;
    if (!load_profile_arg(baseline_path, profile_path, profile, err)) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }
    if (threshold > 0.0) {
        profile.threshold = threshold; // CLI override (extension-2 §5)
    } else if (!baseline_path.empty()) {
        profile.threshold = 2.5; // --baseline compatibility default
    }
    const bool with_categories = !profile_path.empty();

    static const char *kDims[8] = {"bright01", "warm01", "ton01",    "atk01",
                                   "tail01",   "loud01", "jitter01", "fluct01"};

    sp::Manifest candidate = sp::scan_directory(dir, sp::ScanOptions{});
    struct Row {
        const sp::FileEntry *entry;
        sp::Deviation dev;
    };
    std::vector<Row> rows;
    rows.reserve(candidate.files.size());

    // Family medians of the perceptual metrics for the §7 JND phrasing.
    auto median_of = [](std::vector<double> v) {
        if (v.empty()) {
            return 0.0;
        }
        std::sort(v.begin(), v.end());
        const std::size_t n = v.size();
        return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    };
    std::vector<double> med_sones, med_acum, med_asper, med_vacil;
    for (const sp::FileEntry &e : candidate.files) {
        if (e.error.empty() && !e.loudness.silent) {
            med_sones.push_back(e.psycho.sones_n5);
            med_acum.push_back(e.psycho.sharpness_acum);
            med_asper.push_back(e.psycho.roughness_asper);
            med_vacil.push_back(e.psycho.fluctuation_vacil);
        }
    }
    const sp::MappingConfig &mc = sp::active_mapping_config();
    const double fam_sones = std::max(median_of(med_sones), 1e-6);
    const double fam_acum = std::max(median_of(med_acum), mc.bright_acum_lo);
    const double fam_asper = std::max(median_of(med_asper), mc.jitter_asper_lo);
    const double fam_vacil = std::max(median_of(med_vacil), mc.fluct_vacil_lo);
    // Perceptual clauses for the offending psycho dims (§7): loudness in JND-of-ratio
    // (count = ln(ratio)/ln(jnd_loud_ratio)), the others in JND-of-fraction of the family
    // median (count = |delta| / (jnd_fraction * median)). Returns text; fills the json map.
    auto jnd_clauses = [&](const Row &r, nlohmann::ordered_json *jnd_json) {
        std::string text;
        const sp::PsychoFeatures &p = r.entry->psycho;
        auto add = [&text](const char *fmt, double a, double b) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), fmt, a, b);
            if (!text.empty()) {
                text += " ";
            }
            text += buf;
        };
        auto offending = [&](int d) {
            // Emit a clause for the dims that actually drive the flag.
            return std::fabs(r.dev.z[static_cast<std::size_t>(d)]) >= 0.8 * profile.threshold;
        };
        if (offending(5)) { // loud01 <- sones
            const double ratio = std::max(p.sones_n5, 1e-6) / fam_sones;
            const double jnd = std::log(std::max(ratio, 1e-9)) / std::log(mc.jnd_loud_ratio);
            add("loudness %.1fx family median (~%.0f JND)", ratio, std::fabs(jnd));
            if (jnd_json != nullptr) {
                (*jnd_json)["loudness"] = std::round(jnd * 10.0) / 10.0;
            }
        }
        if (offending(0)) { // bright01 <- acum
            const double delta = p.sharpness_acum - fam_acum;
            const double jnd = delta / (mc.jnd_fraction * fam_acum);
            add("sharpness %+.1f acum (~%.0f JND)", delta, std::fabs(jnd));
            if (jnd_json != nullptr) {
                (*jnd_json)["sharpness"] = std::round(jnd * 10.0) / 10.0;
            }
        }
        if (offending(6)) { // jitter01 <- asper
            const double delta = p.roughness_asper - fam_asper;
            const double jnd = delta / (mc.jnd_fraction * fam_asper);
            add("roughness %+.2f asper (~%.0f JND)", delta, std::fabs(jnd));
            if (jnd_json != nullptr) {
                (*jnd_json)["roughness"] = std::round(jnd * 10.0) / 10.0;
            }
        }
        if (offending(7)) { // fluct01 <- vacil
            const double delta = p.fluctuation_vacil - fam_vacil;
            const double jnd = delta / (mc.jnd_fraction * fam_vacil);
            add("fluctuation %+.2f vacil (~%.0f JND)", delta, std::fabs(jnd));
            if (jnd_json != nullptr) {
                (*jnd_json)["fluctuation"] = std::round(jnd * 10.0) / 10.0;
            }
        }
        return text;
    };
    for (const sp::FileEntry &e : candidate.files) {
        if (e.error.empty() && !e.loudness.silent) {
            rows.push_back({&e, sp::compute_deviation(e, profile)});
        }
    }
    std::vector<const Row *> outliers;
    for (const Row &r : rows) {
        if (r.dev.max_z >= profile.threshold) {
            outliers.push_back(&r);
        }
    }
    std::sort(outliers.begin(), outliers.end(),
              [](const Row *a, const Row *b) { return a->dev.max_z > b->dev.max_z; });

    auto dims_over_of = [&](const Row &r) {
        std::vector<std::string> dims;
        for (int d = 0; d < 8; ++d) {
            if (std::fabs(r.dev.z[static_cast<std::size_t>(d)]) >= profile.threshold) {
                dims.emplace_back(kDims[d]);
            }
        }
        return dims;
    };

    if (json_out) {
        // Canonical JSON per PLAN.md §8 rules: fixed key order, 4-decimal rounding.
        nlohmann::ordered_json j;
        j["pass"] = outliers.empty();
        j["threshold"] = round4(profile.threshold);
        j["outliers"] = nlohmann::ordered_json::array();
        int listed = 0;
        for (const Row *r : outliers) {
            if (listed >= top) {
                break;
            }
            nlohmann::ordered_json o;
            o["path"] = r->entry->path;
            o["category"] = r->dev.category;
            o["max_z"] = round4(r->dev.max_z);
            o["worst_dim"] = kDims[r->dev.worst_dim];
            o["dims_over"] = dims_over_of(*r);
            nlohmann::ordered_json jnd = nlohmann::ordered_json::object();
            jnd_clauses(*r, &jnd);
            o["jnd"] = std::move(jnd);
            j["outliers"].push_back(std::move(o));
            ++listed;
        }
        if (json_all) {
            // --all: every non-error, non-silent file with its full z vector (§6.1).
            j["files"] = nlohmann::ordered_json::array();
            for (const Row &r : rows) {
                nlohmann::ordered_json o;
                o["path"] = r.entry->path;
                o["category"] = r.dev.category;
                o["max_z"] = round4(r.dev.max_z);
                o["worst_dim"] = kDims[r.dev.worst_dim];
                o["band"] = sp::dev_band_name(r.dev.band);
                nlohmann::ordered_json z;
                for (int d = 0; d < 8; ++d) {
                    z[kDims[d]] = round4(r.dev.z[static_cast<std::size_t>(d)]);
                }
                o["z"] = std::move(z);
                j["files"].push_back(std::move(o));
            }
        }
        std::fputs(j.dump(2).c_str(), stdout);
        std::fputc('\n', stdout);
        return outliers.empty() ? 0 : 1;
    }

    if (outliers.empty()) {
        std::fprintf(stdout, "PASS %d files within palette\n", static_cast<int>(rows.size()));
        return 0;
    }

    int shown = 0;
    for (const Row *r : outliers) {
        if (shown >= top) {
            break;
        }
        std::string dims;
        for (const std::string &d : dims_over_of(*r)) {
            if (!dims.empty()) {
                dims += ",";
            }
            dims += d;
        }
        const double worst_z = r->dev.z[static_cast<std::size_t>(r->dev.worst_dim)];
        const std::string jnd = jnd_clauses(*r, nullptr);
        if (with_categories) {
            std::fprintf(stdout, "OUTLIER %s cat=%s %s[z %+.2f] worst=%s dims=%s\n",
                         r->entry->path.c_str(), r->dev.category.c_str(),
                         jnd.empty() ? "" : (jnd + " ").c_str(), worst_z, kDims[r->dev.worst_dim],
                         dims.c_str());
        } else {
            std::fprintf(stdout, "OUTLIER %s %s[z %+.2f] worst=%s dims=%s\n",
                         r->entry->path.c_str(), jnd.empty() ? "" : (jnd + " ").c_str(), worst_z,
                         kDims[r->dev.worst_dim], dims.c_str());
        }
        ++shown;
    }
    return 1;
}

int cmd_describe(const std::vector<std::string> &args) {
    std::string file;
    bool json_out = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--json") {
            json_out = true;
        } else if (args[i].rfind("--", 0) != 0 && file.empty()) {
            file = args[i];
        }
    }
    if (file.empty()) {
        std::fprintf(stderr, "usage: soundpalette describe <file> [--json]\n");
        return 2;
    }
    std::filesystem::path p(file);
    if (!std::filesystem::exists(p) || !std::filesystem::is_regular_file(p)) {
        std::fprintf(stderr, "soundpalette: not a file: %s\n", file.c_str());
        return 2;
    }

    sp::FileEntry e = sp::analyze_file(p.parent_path(), p);
    if (!e.error.empty()) {
        std::fprintf(stderr, "soundpalette: %s: %s\n", file.c_str(), e.error.c_str());
        return 2;
    }
    std::string sentence = sp::describe_words(e.features, e.loudness, e.psycho);
    if (!e.loudness.silent) {
        // §7: the sentence carries the perceptual units so a human can read them.
        char units[96];
        std::snprintf(units, sizeof(units), " %.1f sones, %.1f acum, %.2f asper.",
                      e.psycho.sones_n5, e.psycho.sharpness_acum, e.psycho.roughness_asper);
        sentence.pop_back(); // replace the trailing '.' with the units clause
        sentence += ";";
        sentence += units;
    }

    if (!json_out) {
        std::fprintf(stdout, "%s\n", sentence.c_str());
        return 0;
    }

    std::array<double, 8> dims = sp::mapping_dims(e.features, e.loudness, e.psycho);
    std::array<std::string, 8> words = sp::describe_dim_words(e.features, e.loudness, e.psycho);
    static const char *kDims[8] = {"bright01", "warm01", "ton01",    "atk01",
                                   "tail01",   "loud01", "jitter01", "fluct01"};

    nlohmann::ordered_json j;
    j["path"] = e.path;
    j["duration_s"] = round4(e.duration_s);
    j["sample_rate"] = e.sample_rate;
    j["channels"] = e.channels;
    j["truncated"] = e.truncated;
    j["loudness"] = {{"lufs_i", round4(e.loudness.lufs_i)},
                     {"true_peak_db", round4(e.loudness.true_peak_db)},
                     {"silent", e.loudness.silent}};
    nlohmann::ordered_json features;
    features["centroid_hz"] = round4(e.features.centroid_hz);
    features["rolloff85_hz"] = round4(e.features.rolloff85_hz);
    features["flatness"] = round4(e.features.flatness);
    features["zcr"] = round4(e.features.zcr);
    features["attack_s"] = round4(e.features.attack_s);
    features["tail_s"] = round4(e.features.tail_s);
    features["tail_clipped"] = e.features.tail_clipped;
    features["roughness"] = round4(e.features.roughness);
    features["warmth"] = round4(e.features.warmth);
    j["features"] = std::move(features);
    nlohmann::ordered_json visual;
    visual["hue_deg"] = round4(e.visual.hue_deg);
    visual["sat"] = round4(e.visual.sat);
    visual["light"] = round4(e.visual.light);
    visual["size_px"] = round4(e.visual.size_px);
    visual["spike01"] = round4(e.visual.spike01);
    visual["spikes"] = e.visual.spikes;
    visual["jitter01"] = round4(e.visual.jitter01);
    visual["tail01"] = round4(e.visual.tail01);
    j["visual"] = std::move(visual);
    nlohmann::ordered_json jd, jw;
    for (int d = 0; d < 8; ++d) {
        jd[kDims[d]] = round4(dims[static_cast<std::size_t>(d)]);
        jw[kDims[d]] = words[static_cast<std::size_t>(d)];
    }
    j["dims"] = std::move(jd);
    j["words"] = std::move(jw);
    j["psycho"] = {{"ref_spl", round4(e.psycho.ref_spl)},
                   {"sones_n5", round4(e.psycho.sones_n5)},
                   {"sones_mean", round4(e.psycho.sones_mean)},
                   {"sharpness_acum", round4(e.psycho.sharpness_acum)},
                   {"roughness_asper", round4(e.psycho.roughness_asper)},
                   {"fluctuation_vacil", round4(e.psycho.fluctuation_vacil)},
                   {"experimental_fluctuation", e.psycho.experimental_fluctuation}};
    // §7 fixed definitional anchor strings: the units are defined as reference signals.
    j["anchors"] = {
        {"1 sone", "level of a 1 kHz tone at 40 dB SPL"},
        {"1 acum", "sharpness of narrowband noise at 1 kHz, 60 dB"},
        {"1 asper", "roughness of a 1 kHz tone, 60 dB, fully amplitude-modulated at 70 Hz"},
        {"1 vacil", "same but modulated at 4 Hz"}};
    j["sentence"] = sentence;

    std::fputs(j.dump(2).c_str(), stdout);
    std::fputc('\n', stdout);
    return 0;
}

// ---- M10 profile subcommands (extension-2 §4.3) ----

// Per-file psychoacoustic metrics as JSON (extension-3 M13). Diagnostic surface: the
// psycho_oracle_match.sh gate compares these numbers against tests/golden/psycho_reference
// within the §5 tolerances; describe/lint integration follows in M14.
int cmd_psycho(const std::vector<std::string> &args) {
    if (args.empty()) {
        std::fprintf(stderr, "usage: soundpalette psycho <file> [<file>...]\n");
        return 2;
    }
    nlohmann::json out = nlohmann::json::object();
    for (const std::string &path : args) {
        std::string err;
        auto buffer = sp::decode_file(path, err);
        if (!buffer.has_value()) {
            std::fprintf(stderr, "soundpalette: %s: %s\n", path.c_str(), err.c_str());
            return 2;
        }
        sp::PsychoFeatures p = sp::compute_psycho(*buffer, sp::active_mapping_config());
        out[path] = {{"ref_spl", round4(p.ref_spl)},
                     {"sones_n5", round4(p.sones_n5)},
                     {"sones_mean", round4(p.sones_mean)},
                     {"sharpness_acum", round4(p.sharpness_acum)},
                     {"roughness_asper", round4(p.roughness_asper)},
                     {"fluctuation_vacil", round4(p.fluctuation_vacil)},
                     {"experimental_fluctuation", p.experimental_fluctuation}};
    }
    std::fprintf(stdout, "%s\n", out.dump(2).c_str());
    return 0;
}

int cmd_profile(const std::vector<std::string> &args) {
    if (args.empty()) {
        std::fprintf(stderr, "usage: soundpalette profile <create|show|preset> ...\n");
        return 2;
    }
    const std::string &verb = args[0];

    if (verb == "show") {
        if (args.size() < 2) {
            std::fprintf(stderr, "usage: soundpalette profile show <file.sppal.json>\n");
            return 2;
        }
        sp::Profile p;
        std::string err;
        if (!load_profile_arg("", args[1], p, err)) {
            std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
            return 2;
        }
        std::fprintf(stdout, "profile %s (v%d, mapping v%d)\n", p.name.c_str(), p.profile_version,
                     p.mapping_version);
        if (!p.description.empty()) {
            std::fprintf(stdout, "  %s\n", p.description.c_str());
        }
        std::fprintf(stdout, "  threshold %.2f, from %s '%s', %d files\n", p.threshold,
                     p.created_from_type.c_str(), p.created_from_root.c_str(),
                     p.created_from_file_count);
        for (const sp::CategoryProfile &c : p.categories) {
            std::string patterns;
            for (std::size_t i = 0; i < c.match.size(); ++i) {
                patterns += (i ? "," : "") + c.match[i];
            }
            std::fprintf(stdout, "  category %s: %d files (%s)%s\n", c.name.c_str(), c.file_count,
                         patterns.c_str(),
                         c.file_count < 5 ? "  [warning: fewer than 5 files]" : "");
        }
        return 0;
    }

    if (verb == "preset") {
        // Built-in designed genre presets (guardrail priors, not corpus stats). `export`
        // materializes one as an ordinary .sppal.json so it composes with every --profile flag.
        if (args.size() >= 2 && args[1] == "list") {
            for (const sp::PresetInfo &info : sp::builtin_preset_list()) {
                std::fprintf(stdout, "%-12s %s — %s\n", info.slug.c_str(), info.name.c_str(),
                             info.description.c_str());
            }
            return 0;
        }
        if (args.size() >= 3 && args[1] == "export") {
            const std::string &slug = args[2];
            std::string out = slug + ".sppal.json";
            for (std::size_t i = 3; i + 1 < args.size(); ++i) {
                if (args[i] == "--out") {
                    out = args[i + 1];
                }
            }
            std::optional<sp::Profile> p = sp::builtin_preset(slug);
            if (!p.has_value()) {
                std::fprintf(stderr,
                             "soundpalette: unknown preset '%s' (see profile preset list)\n",
                             slug.c_str());
                return 2;
            }
            std::ofstream f(out, std::ios::binary);
            if (!f) {
                std::fprintf(stderr, "soundpalette: cannot write %s\n", out.c_str());
                return 2;
            }
            f << sp::profile_to_json(*p) << "\n";
            std::fprintf(stdout, "wrote %s\n", out.c_str());
            return 0;
        }
        std::fprintf(stderr,
                     "usage: soundpalette profile preset list | preset export <slug> [--out f]\n");
        return 2;
    }

    if (verb != "create") {
        std::fprintf(stderr, "soundpalette: unknown profile verb '%s'\n", verb.c_str());
        return 2;
    }
    if (!sp::capability("profile.create")) {
        std::fprintf(stderr, "soundpalette: profile.create is not available\n");
        return 2;
    }

    std::string dir, name, out, from_manifest, select_file, description;
    double threshold = 2.5;
    std::vector<std::pair<std::string, std::vector<std::string>>> categories;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--name" && i + 1 < args.size()) {
            name = args[++i];
        } else if (a == "--out" && i + 1 < args.size()) {
            out = args[++i];
        } else if (a == "--from-manifest" && i + 1 < args.size()) {
            from_manifest = args[++i];
        } else if (a == "--select" && i + 1 < args.size()) {
            select_file = args[++i];
        } else if (a == "--threshold" && i + 1 < args.size()) {
            threshold = std::stod(args[++i]);
        } else if (a == "--description" && i + 1 < args.size()) {
            description = args[++i];
        } else if (a == "--category" && i + 1 < args.size()) {
            // --category name="pat1,pat2"
            std::string spec = args[++i];
            std::size_t eq = spec.find('=');
            if (eq == std::string::npos) {
                std::fprintf(stderr, "soundpalette: bad --category (want name=\"globs\")\n");
                return 2;
            }
            std::string cat_name = spec.substr(0, eq);
            std::vector<std::string> patterns;
            std::string rest = spec.substr(eq + 1);
            std::size_t pos = 0;
            while (pos <= rest.size()) {
                std::size_t comma = rest.find(',', pos);
                std::string pat =
                    rest.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                if (!pat.empty()) {
                    patterns.push_back(pat);
                }
                if (comma == std::string::npos) {
                    break;
                }
                pos = comma + 1;
            }
            categories.emplace_back(cat_name, std::move(patterns));
        } else if (a.rfind("--", 0) != 0 && dir.empty()) {
            dir = a;
        }
    }
    if (name.empty() || (dir.empty() && from_manifest.empty())) {
        std::fprintf(stderr,
                     "usage: soundpalette profile create <dir> --name X [--out X.sppal.json]\n"
                     "         [--category ui=\"ui/**,**/ui_*\"] [--from-manifest m.json]\n"
                     "         [--select files.txt] [--threshold 2.5] [--description ...]\n");
        return 2;
    }

    std::vector<sp::FileEntry> entries;
    std::string root;
    if (!from_manifest.empty()) {
        // Reuse an existing scan instead of re-analyzing (§4.3).
        std::ifstream f(from_manifest);
        if (!f) {
            std::fprintf(stderr, "soundpalette: cannot open %s\n", from_manifest.c_str());
            return 2;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(ss.str());
        } catch (const std::exception &e) {
            std::fprintf(stderr, "soundpalette: %s: %s\n", from_manifest.c_str(), e.what());
            return 2;
        }
        root = j.value("root", "");
        for (const auto &fj : j.at("files")) {
            sp::FileEntry e;
            e.path = fj.value("path", "");
            e.error = fj.value("error", "");
            if (!e.error.empty()) {
                continue;
            }
            const auto &lj = fj.at("loudness");
            e.loudness.lufs_i = lj.value("lufs_i", 0.0);
            e.loudness.silent = lj.value("silent", false);
            const auto &tj = fj.at("features");
            e.features.centroid_hz = tj.value("centroid_hz", 0.0);
            e.features.flatness = tj.value("flatness", 0.0);
            e.features.attack_s = tj.value("attack_s", 0.0);
            e.features.tail_s = tj.value("tail_s", 0.0);
            e.features.roughness = tj.value("roughness", 0.0);
            e.features.warmth = tj.value("warmth", 0.0);
            entries.push_back(std::move(e));
        }
    } else {
        sp::Manifest m = sp::scan_directory(dir, sp::ScanOptions{});
        entries = std::move(m.files);
        root = dir;
    }

    std::string created_type = from_manifest.empty() ? "folder" : "manifest";
    if (!select_file.empty()) {
        // Curated selection (§4.3): keep only the listed manifest-relative paths.
        std::ifstream f(select_file);
        if (!f) {
            std::fprintf(stderr, "soundpalette: cannot open %s\n", select_file.c_str());
            return 2;
        }
        std::vector<std::string> wanted;
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty()) {
                wanted.push_back(line);
            }
        }
        std::vector<sp::FileEntry> selected;
        for (sp::FileEntry &e : entries) {
            if (std::find(wanted.begin(), wanted.end(), e.path) != wanted.end()) {
                selected.push_back(std::move(e));
            }
        }
        entries = std::move(selected);
        created_type = "selection";
    }

    sp::Profile profile =
        sp::profile_from_entries(entries, name, description, threshold, categories);
    profile.created_from_type = created_type;
    profile.created_from_root = root;

    for (const sp::CategoryProfile &c : profile.categories) {
        if (c.file_count < 5) {
            std::fprintf(stderr, "warning: category '%s' has fewer than 5 files (%d)\n",
                         c.name.c_str(), c.file_count);
        }
    }

    return write_or_print(out, sp::profile_to_json(profile)) ? 0 : 2;
}

// ---- M9 recipe engine subcommands (extension §6.5) ----

bool analyze_for_propose(const std::string &file, sp::NativeAudio &audio, sp::Loudness &loudness,
                         sp::Features &features, sp::PsychoFeatures &psycho) {
    std::string err;
    auto decoded = sp::decode_file_native(file, err);
    if (!decoded.has_value()) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return false;
    }
    audio = std::move(*decoded);
    sp::analyze_native(audio, loudness, features, psycho);
    return true;
}

int cmd_propose(const std::vector<std::string> &args) {
    std::string file, baseline_path, profile_path, out;
    double threshold = -1.0;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--baseline" && i + 1 < args.size()) {
            baseline_path = args[++i];
        } else if (a == "--profile" && i + 1 < args.size()) {
            profile_path = args[++i];
        } else if (a == "--threshold" && i + 1 < args.size()) {
            threshold = std::stod(args[++i]);
        } else if (a == "--out" && i + 1 < args.size()) {
            out = args[++i];
        } else if (a.rfind("--", 0) != 0 && file.empty()) {
            file = a;
        }
    }
    if (file.empty() || (baseline_path.empty() && profile_path.empty())) {
        std::fprintf(stderr, "usage: soundpalette propose <file> (--baseline m.json | --profile "
                             "p.sppal.json) [--threshold 2.5] [--out recipe.json]\n");
        return 2;
    }

    sp::Profile profile;
    std::string err;
    if (!load_profile_arg(baseline_path, profile_path, profile, err)) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }
    if (threshold > 0.0) {
        profile.threshold = threshold;
    } else if (!baseline_path.empty()) {
        profile.threshold = 2.5;
    }

    sp::NativeAudio audio;
    sp::Loudness loudness;
    sp::Features features;
    sp::PsychoFeatures psycho;
    if (!analyze_for_propose(file, audio, loudness, features, psycho)) {
        return 2;
    }

    // Category targeting (extension-2 §6.1): pull target stats from the file's resolved
    // category so a misfiled sound harmonizes toward the family it sits in.
    const std::string rel = std::filesystem::path(file).filename().string();
    const int cat = sp::resolve_category(profile, rel);
    const std::array<sp::DimStats, 8> &target_stats =
        cat >= 0 ? profile.categories[static_cast<std::size_t>(cat)].stats : profile.stats;

    sp::Recipe recipe =
        sp::propose_recipe(audio, features, loudness, psycho, target_stats, profile.threshold);
    recipe.source_path = file;
    recipe.source_sha256 = sp::file_sha256(file);
    recipe.target_baseline = profile_path.empty() ? baseline_path : profile_path;

    return write_or_print(out, sp::recipe_to_json(recipe)) ? 0 : 2;
}

int cmd_apply(const std::vector<std::string> &args) {
    std::string file, recipe_path, out, report_path;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--recipe" && i + 1 < args.size()) {
            recipe_path = args[++i];
        } else if (a == "--out" && i + 1 < args.size()) {
            out = args[++i];
        } else if (a == "--report" && i + 1 < args.size()) {
            report_path = args[++i];
        } else if (a.rfind("--", 0) != 0 && file.empty()) {
            file = a;
        }
    }
    if (file.empty() || recipe_path.empty() || out.empty()) {
        std::fprintf(stderr, "usage: soundpalette apply <file> --recipe recipe.json "
                             "--out <file.wav> [--report report.json]\n");
        return 2;
    }

    if (!sp::capability("harmonize.apply")) {
        std::fprintf(stderr, "soundpalette: harmonize.apply is not available\n");
        return 2;
    }

    std::ifstream rf(recipe_path);
    if (!rf) {
        std::fprintf(stderr, "soundpalette: cannot open %s\n", recipe_path.c_str());
        return 2;
    }
    std::ostringstream rss;
    rss << rf.rdbuf();
    std::string err;
    auto recipe = sp::recipe_from_json(rss.str(), err);
    if (!recipe.has_value()) {
        std::fprintf(stderr, "soundpalette: %s: %s\n", recipe_path.c_str(), err.c_str());
        return 2;
    }

    auto audio = sp::decode_file_native(file, err);
    if (!audio.has_value()) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }

    sp::ApplyReport apply_report;
    sp::apply_chain(*audio, recipe->ops, apply_report);
    if (!sp::write_wav_f32(out, *audio, err)) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }

    if (!report_path.empty()) {
        sp::Loudness post_loudness;
        sp::Features post_features;
        sp::PsychoFeatures post_psycho;
        sp::analyze_native(*audio, post_loudness, post_features, post_psycho);
        nlohmann::ordered_json j;
        j["source"] = file;
        j["output"] = out;
        j["limited_by_peak"] = apply_report.limited_by_peak;
        j["post"] = {{"lufs_i", round4(post_loudness.lufs_i)},
                     {"true_peak_db", round4(post_loudness.true_peak_db)},
                     {"centroid_hz", round4(post_features.centroid_hz)},
                     {"flatness", round4(post_features.flatness)},
                     {"attack_s", round4(post_features.attack_s)},
                     {"tail_s", round4(post_features.tail_s)},
                     {"warmth", round4(post_features.warmth)}};
        if (!write_or_print(report_path, j.dump(2))) {
            return 2;
        }
    }
    return 0;
}

int cmd_harmonize(const std::vector<std::string> &args) {
    std::string target, baseline_path, profile_path, out_dir = "harmonized";
    double threshold = -1.0;
    int max_iter = 3;
    bool dry_run = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--baseline" && i + 1 < args.size()) {
            baseline_path = args[++i];
        } else if (a == "--profile" && i + 1 < args.size()) {
            profile_path = args[++i];
        } else if (a == "--out-dir" && i + 1 < args.size()) {
            out_dir = args[++i];
        } else if (a == "--threshold" && i + 1 < args.size()) {
            threshold = std::stod(args[++i]);
        } else if (a == "--max-iter" && i + 1 < args.size()) {
            max_iter = std::stoi(args[++i]);
        } else if (a == "--dry-run") {
            dry_run = true;
        } else if (a.rfind("--", 0) != 0 && target.empty()) {
            target = a;
        }
    }
    if (target.empty() || (baseline_path.empty() && profile_path.empty())) {
        std::fprintf(stderr,
                     "usage: soundpalette harmonize <dir|file> (--baseline m.json | --profile "
                     "p.sppal.json) [--out-dir harmonized] [--threshold 2.5] [--max-iter 3] "
                     "[--dry-run]\n");
        return 2;
    }
    if (!dry_run && !sp::capability("harmonize.apply")) {
        std::fprintf(stderr, "soundpalette: harmonize.apply is not available\n");
        return 2;
    }

    sp::Profile profile;
    std::string err;
    if (!load_profile_arg(baseline_path, profile_path, profile, err)) {
        std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
        return 2;
    }
    if (threshold > 0.0) {
        profile.threshold = threshold;
    } else if (!baseline_path.empty()) {
        profile.threshold = 2.5;
    }
    threshold = profile.threshold;

    // Work list: every outlier of a directory, or the single file as given (§6.5). Outliers
    // are found with the same compute_deviation every other surface uses.
    std::vector<std::pair<std::string, std::string>> work; // (absolute-ish path, relpath)
    if (std::filesystem::is_directory(target)) {
        sp::Manifest candidate = sp::scan_directory(target, sp::ScanOptions{});
        struct Flagged {
            std::string path;
            double max_z;
        };
        std::vector<Flagged> flagged;
        for (const sp::FileEntry &e : candidate.files) {
            if (!e.error.empty() || e.loudness.silent) {
                continue;
            }
            sp::Deviation dev = sp::compute_deviation(e, profile);
            if (dev.max_z >= threshold) {
                flagged.push_back({e.path, dev.max_z});
            }
        }
        std::sort(flagged.begin(), flagged.end(),
                  [](const Flagged &a, const Flagged &b) { return a.max_z > b.max_z; });
        for (const Flagged &f : flagged) {
            work.emplace_back((std::filesystem::path(target) / f.path).string(), f.path);
        }
    } else if (std::filesystem::is_regular_file(target)) {
        work.emplace_back(target, std::filesystem::path(target).filename().string());
    } else {
        std::fprintf(stderr, "soundpalette: no such file or directory: %s\n", target.c_str());
        return 2;
    }

    bool all_within = true;
    for (const auto &[abs_path, rel_path] : work) {
        sp::NativeAudio audio;
        sp::Loudness loudness;
        sp::Features features;
        sp::PsychoFeatures psycho;
        if (!analyze_for_propose(abs_path, audio, loudness, features, psycho)) {
            return 2;
        }

        const int cat = sp::resolve_category(profile, rel_path);
        const std::array<sp::DimStats, 8> &target_stats =
            cat >= 0 ? profile.categories[static_cast<std::size_t>(cat)].stats : profile.stats;
        sp::Recipe recipe = sp::propose_recipe(audio, features, loudness, psycho, target_stats,
                                               threshold, max_iter);
        recipe.source_path = rel_path;
        recipe.source_sha256 = sp::file_sha256(abs_path);
        recipe.target_baseline = profile_path.empty() ? baseline_path : profile_path;

        std::filesystem::path rel(rel_path);
        std::filesystem::path out_base = std::filesystem::path(out_dir) / rel.parent_path();
        std::error_code ec;
        std::filesystem::create_directories(out_base, ec);
        std::string stem = rel.stem().string();
        std::filesystem::path wav_out = out_base / (stem + ".harmonized.wav");
        std::filesystem::path recipe_out = out_base / (stem + ".harmonized.recipe.json");

        {
            std::ofstream f(recipe_out, std::ios::binary);
            f << sp::recipe_to_json(recipe) << "\n";
        }
        if (!dry_run && !recipe.ops.empty()) {
            sp::NativeAudio processed = audio;
            sp::ApplyReport apply_report;
            sp::apply_chain(processed, recipe.ops, apply_report);
            if (!sp::write_wav_f32(wav_out, processed, err)) {
                std::fprintf(stderr, "soundpalette: %s\n", err.c_str());
                return 2;
            }
        }

        if (recipe.result_max_z_after <= threshold) {
            std::fprintf(stdout, "HARMONIZED %s max_z %.2f -> %.2f\n", rel_path.c_str(),
                         recipe.result_max_z_before, recipe.result_max_z_after);
        } else {
            all_within = false;
            std::string dims;
            for (std::size_t i = 0; i < recipe.result_unresolved.size(); ++i) {
                if (i > 0) {
                    dims += ",";
                }
                dims += recipe.result_unresolved[i];
            }
            std::fprintf(stdout, "UNRESOLVED %s dims=%s\n", rel_path.c_str(), dims.c_str());
        }
    }
    return all_within ? 0 : 1;
}

int cmd_export_svg(const std::vector<std::string> &args) {
    std::string manifest_path;
    bool with_legend = true;
    std::string out;
    std::string profile_path;
    int columns = 8;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--out" && i + 1 < args.size()) {
            out = args[++i];
        } else if (a == "--columns" && i + 1 < args.size()) {
            columns = std::stoi(args[++i]);
        } else if (a == "--profile" && i + 1 < args.size()) {
            profile_path = args[++i];
        } else if (a == "--no-legend") {
            with_legend = false;
        } else if (a.rfind("--", 0) != 0 && manifest_path.empty()) {
            manifest_path = a;
        }
    }
    // Gating seam site (extension-2 §6.3): a clean sheet (no made-with mark; the mark is not
    // implemented yet) requires this capability. v3: always granted.
    if (!sp::capability("export.clean_sheet")) {
        std::fprintf(stderr, "soundpalette: export.clean_sheet is not available\n");
        return 2;
    }

    if (manifest_path.empty() || out.empty()) {
        std::fprintf(stderr, "usage: soundpalette export-svg <palette.json> --out sheet.svg "
                             "[--columns 8] [--profile p.sppal.json] [--no-legend]\n");
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
        if (fe.error.empty() && fj.contains("loudness") && fj.contains("features")) {
            const auto &lj = fj["loudness"];
            fe.loudness.lufs_i = lj.value("lufs_i", 0.0);
            fe.loudness.true_peak_db = lj.value("true_peak_db", 0.0);
            fe.loudness.silent = lj.value("silent", false);
            const auto &tj = fj["features"];
            fe.features.centroid_hz = tj.value("centroid_hz", 0.0);
            fe.features.flatness = tj.value("flatness", 0.0);
            fe.features.attack_s = tj.value("attack_s", 0.0);
            fe.features.tail_s = tj.value("tail_s", 0.0);
            fe.features.roughness = tj.value("roughness", 0.0);
            fe.features.warmth = tj.value("warmth", 0.0);
        }
        if (fe.error.empty()) {
            // Schema 2 psycho block: without it the v2 dims are meaningless, so refuse (§0).
            if (!fj.contains("psycho")) {
                std::fprintf(stderr,
                             "soundpalette: %s: no psycho block (schema 1 manifest? rescan "
                             "to regenerate)\n",
                             manifest_path.c_str());
                return 2;
            }
            const auto &pj = fj["psycho"];
            fe.psycho.ref_spl = pj.value("ref_spl", 75.0);
            fe.psycho.sones_n5 = pj.value("sones_n5", 0.0);
            fe.psycho.sones_mean = pj.value("sones_mean", 0.0);
            fe.psycho.sharpness_acum = pj.value("sharpness_acum", 0.0);
            fe.psycho.roughness_asper = pj.value("roughness_asper", 0.0);
            fe.psycho.fluctuation_vacil = pj.value("fluctuation_vacil", 0.0);
            fe.psycho.experimental_fluctuation = pj.value("experimental_fluctuation", true);
        }
        if (fe.error.empty() && fj.contains("visual")) {
            const auto &vj = fj["visual"];
            fe.visual.hue_deg = vj.value("hue_deg", 0.0);
            fe.visual.sat = vj.value("sat", 0.0);
            fe.visual.light = vj.value("light", 0.0);
            fe.visual.size_px = vj.value("size_px", 0.0);
            fe.visual.ton01 = vj.value("ton01", 0.5);
            fe.visual.spike01 = vj.value("spike01", 0.0);
            fe.visual.spikes = vj.value("spikes", 0);
            fe.visual.jitter01 = vj.value("jitter01", 0.0);
            fe.visual.tail01 = vj.value("tail01", 0.0);
            fe.visual.fluct01 = vj.value("fluct01", 0.0);
            fe.visual.loud01 = vj.value("loud01", 0.0);
            fe.visual.sharp01 = vj.value("sharp01", 0.0);
            fe.visual.silent = vj.value("silent", false);
            fe.visual.seed = vj.value("seed", static_cast<std::uint64_t>(0));
        }
        manifest.files.push_back(std::move(fe));
    }

    std::string svg;
    if (!profile_path.empty()) {
        sp::Profile profile;
        std::string perr;
        if (!load_profile_arg("", profile_path, profile, perr)) {
            std::fprintf(stderr, "soundpalette: %s\n", perr.c_str());
            return 2;
        }
        svg = sp::sheet_svg(manifest, columns, profile, with_legend);
    } else {
        svg = sp::sheet_svg(manifest, columns, with_legend);
    }
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
        std::fprintf(
            stderr,
            "usage: soundpalette "
            "<scan|lint|describe|profile|propose|apply|harmonize|export-svg|watch|print-mapping> "
            "[args...]\n");
        return 2;
    }

    std::string subcommand = argv[1];
    std::vector<std::string> args = to_vec(argc, argv, 2);

    try {
        if (subcommand == "scan") {
            return cmd_scan(args);
        } else if (subcommand == "print-mapping") {
            return cmd_print_mapping(args);
        } else if (subcommand == "psycho") {
            return cmd_psycho(args);
        } else if (subcommand == "profile") {
            return cmd_profile(args);
        } else if (subcommand == "propose") {
            return cmd_propose(args);
        } else if (subcommand == "apply") {
            return cmd_apply(args);
        } else if (subcommand == "harmonize") {
            return cmd_harmonize(args);
        } else if (subcommand == "describe") {
            return cmd_describe(args);
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
