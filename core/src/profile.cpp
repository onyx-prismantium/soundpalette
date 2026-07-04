// Palette profiles: schema, IO, glob matcher, category resolution (extension-2 §4).

#include <algorithm>
#include <cmath>
#include <regex>

#include <nlohmann/json.hpp>

#include "soundpalette/capability.h"
#include "soundpalette/mapping.h"
#include "soundpalette/profile.h"

namespace sp {

namespace {

constexpr const char *kDimNames[7] = {"bright01", "warm01", "ton01",   "atk01",
                                      "tail01",   "loud01", "jitter01"};

double round6(double x) {
    return std::round(x * 1000000.0) / 1000000.0;
}

// §4.2: translate a glob to an anchored ECMAScript regex. ** spans segments, * stays within
// one segment, ? is a single non-slash character; everything else is literal.
std::string glob_to_regex(std::string_view pattern) {
    std::string re = "^";
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                re += ".*";
                ++i;
            } else {
                re += "[^/]*";
            }
        } else if (c == '?') {
            re += "[^/]";
        } else if (std::string("\\^$.|+()[]{}").find(c) != std::string::npos) {
            re += '\\';
            re += c;
        } else {
            re += c;
        }
    }
    re += "$";
    return re;
}

// Stats + covariance over a set of dim vectors (population forms, matching the §8 manifest
// stats block; diagonal of cov is exactly std² by construction).
void compute_stats_cov(const std::vector<std::array<double, 7>> &dims,
                       std::array<DimStats, 7> &stats, Cov7 &cov) {
    stats = {};
    cov = {};
    const std::size_t n = dims.size();
    if (n == 0) {
        return;
    }
    std::array<double, 7> mean{};
    for (const auto &v : dims) {
        for (std::size_t d = 0; d < 7; ++d) {
            mean[d] += v[d];
        }
    }
    for (std::size_t d = 0; d < 7; ++d) {
        mean[d] /= static_cast<double>(n);
    }
    for (std::size_t d = 0; d < 7; ++d) {
        stats[d].mean = mean[d];
        stats[d].min = dims[0][d];
        stats[d].max = dims[0][d];
    }
    for (const auto &v : dims) {
        for (std::size_t a = 0; a < 7; ++a) {
            stats[a].min = std::min(stats[a].min, v[a]);
            stats[a].max = std::max(stats[a].max, v[a]);
            for (std::size_t b = 0; b < 7; ++b) {
                cov[a][b] += (v[a] - mean[a]) * (v[b] - mean[b]);
            }
        }
    }
    for (std::size_t a = 0; a < 7; ++a) {
        for (std::size_t b = 0; b < 7; ++b) {
            cov[a][b] /= static_cast<double>(n);
        }
        stats[a].std = std::sqrt(cov[a][a]);
    }
}

nlohmann::ordered_json stats_to_json(const std::array<DimStats, 7> &stats) {
    nlohmann::ordered_json j;
    for (std::size_t d = 0; d < 7; ++d) {
        j[kDimNames[d]] = {{"mean", round6(stats[d].mean)},
                           {"std", round6(stats[d].std)},
                           {"min", round6(stats[d].min)},
                           {"max", round6(stats[d].max)}};
    }
    return j;
}

nlohmann::ordered_json cov_to_json(const Cov7 &cov) {
    nlohmann::ordered_json rows = nlohmann::ordered_json::array();
    for (std::size_t a = 0; a < 7; ++a) {
        nlohmann::ordered_json row = nlohmann::ordered_json::array();
        for (std::size_t b = 0; b < 7; ++b) {
            row.push_back(round6(cov[a][b]));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

bool stats_from_json(const nlohmann::json &j, std::array<DimStats, 7> &stats, std::string &err) {
    for (std::size_t d = 0; d < 7; ++d) {
        if (!j.contains(kDimNames[d])) {
            err = std::string("stats missing ") + kDimNames[d];
            return false;
        }
        const auto &s = j[kDimNames[d]];
        stats[d].mean = s.value("mean", 0.0);
        stats[d].std = s.value("std", 0.0);
        stats[d].min = s.value("min", 0.0);
        stats[d].max = s.value("max", 0.0);
    }
    return true;
}

bool cov_from_json(const nlohmann::json &j, Cov7 &cov, std::string &err) {
    if (!j.is_array() || j.size() != 7) {
        err = "cov must be a 7x7 array";
        return false;
    }
    for (std::size_t a = 0; a < 7; ++a) {
        if (!j[a].is_array() || j[a].size() != 7) {
            err = "cov must be a 7x7 array";
            return false;
        }
        for (std::size_t b = 0; b < 7; ++b) {
            cov[a][b] = j[a][b].get<double>();
        }
    }
    return true;
}

// §4.1: diagonal must equal std² within 1e-9 in memory. Parsed files carry 6-decimal rounding
// on cov and std independently (§4.1 serialization), so validation of serialized input allows
// the corresponding worst-case rounding noise (~2*std*5e-7 + 5e-7) on top.
bool validate_cov(const std::array<DimStats, 7> &stats, const Cov7 &cov, std::string &err) {
    for (std::size_t d = 0; d < 7; ++d) {
        const double tolerance = 1e-9 + 5e-7 + 2.0 * stats[d].std * 5e-7;
        if (std::fabs(cov[d][d] - stats[d].std * stats[d].std) > tolerance) {
            err = std::string("cov diagonal does not match std^2 for ") + kDimNames[d];
            return false;
        }
    }
    return true;
}

} // namespace

bool capability(std::string_view feature) {
    (void)feature; // v3: everything unlocked, unknown names included (fail-open, §6.3)
    return true;
}

bool glob_match(std::string_view pattern, std::string_view path) {
    const std::regex re(glob_to_regex(pattern));
    return std::regex_match(path.begin(), path.end(), re);
}

int resolve_category(const Profile &profile, std::string_view path) {
    for (std::size_t c = 0; c < profile.categories.size(); ++c) {
        for (const std::string &pattern : profile.categories[c].match) {
            if (glob_match(pattern, path)) {
                return static_cast<int>(c); // first category with any match wins (§4.2)
            }
        }
    }
    return -1;
}

Profile profile_from_manifest(const Manifest &manifest) {
    Profile p;
    p.name = "";
    p.created_from_type = "manifest";
    p.created_from_root = manifest.root;
    p.stats = manifest.stats;
    // The manifest stats block has no covariance; synthesize the diagonal so cov stays
    // schema-valid (off-diagonals unknown -> 0). Deviation only needs the per-dim stats.
    for (std::size_t d = 0; d < 7; ++d) {
        p.cov[d][d] = manifest.stats[d].std * manifest.stats[d].std;
    }
    int count = 0;
    for (const FileEntry &e : manifest.files) {
        if (e.error.empty() && !e.loudness.silent) {
            ++count;
        }
    }
    p.created_from_file_count = count;
    return p;
}

Profile profile_from_entries(
    const std::vector<FileEntry> &entries, const std::string &name, const std::string &description,
    double threshold,
    const std::vector<std::pair<std::string, std::vector<std::string>>> &category_specs) {
    Profile p;
    p.name = name;
    p.description = description;
    p.threshold = threshold;

    for (const auto &[cat_name, patterns] : category_specs) {
        CategoryProfile cat;
        cat.name = cat_name;
        cat.match = patterns;
        p.categories.push_back(std::move(cat));
    }

    std::vector<std::array<double, 7>> all_dims;
    std::vector<std::vector<std::array<double, 7>>> cat_dims(p.categories.size());
    for (const FileEntry &e : entries) {
        if (!e.error.empty() || e.loudness.silent) {
            continue; // §4.1: silent and error files never contribute
        }
        std::array<double, 7> dims = mapping_dims(e.features, e.loudness);
        all_dims.push_back(dims);
        int cat = resolve_category(p, e.path);
        if (cat >= 0) {
            cat_dims[static_cast<std::size_t>(cat)].push_back(dims);
        }
    }

    p.created_from_file_count = static_cast<int>(all_dims.size());
    compute_stats_cov(all_dims, p.stats, p.cov);
    for (std::size_t c = 0; c < p.categories.size(); ++c) {
        p.categories[c].file_count = static_cast<int>(cat_dims[c].size());
        compute_stats_cov(cat_dims[c], p.categories[c].stats, p.categories[c].cov);
    }
    return p;
}

std::string profile_to_json(const Profile &p) {
    using json = nlohmann::ordered_json;
    json j;
    j["profile_version"] = p.profile_version;
    j["mapping_version"] = p.mapping_version;
    j["name"] = p.name;
    j["description"] = p.description;
    j["threshold"] = round6(p.threshold);
    j["created_from"] = {{"type", p.created_from_type},
                         {"root", p.created_from_root},
                         {"file_count", p.created_from_file_count}};
    j["stats"] = stats_to_json(p.stats);
    j["cov"] = cov_to_json(p.cov);
    json cats = json::array();
    for (const CategoryProfile &c : p.categories) {
        json cj;
        cj["name"] = c.name;
        cj["match"] = c.match;
        cj["file_count"] = c.file_count;
        cj["stats"] = stats_to_json(c.stats);
        cj["cov"] = cov_to_json(c.cov);
        cats.push_back(std::move(cj));
    }
    j["categories"] = std::move(cats);
    return j.dump(2);
}

std::optional<Profile> profile_from_json(const std::string &text, std::string &err) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception &e) {
        err = e.what();
        return std::nullopt;
    }

    Profile p;
    try {
        p.profile_version = j.at("profile_version").get<int>();
        if (p.profile_version != 1) {
            err = "unsupported profile_version";
            return std::nullopt;
        }
        p.mapping_version = j.value("mapping_version", 1);
        p.name = j.value("name", "");
        p.description = j.value("description", "");
        p.threshold = j.value("threshold", 2.5);
        if (j.contains("created_from")) {
            p.created_from_type = j["created_from"].value("type", "folder");
            p.created_from_root = j["created_from"].value("root", "");
            p.created_from_file_count = j["created_from"].value("file_count", 0);
        }
        if (!stats_from_json(j.at("stats"), p.stats, err) ||
            !cov_from_json(j.at("cov"), p.cov, err) || !validate_cov(p.stats, p.cov, err)) {
            return std::nullopt;
        }
        for (const auto &cj : j.value("categories", nlohmann::json::array())) {
            CategoryProfile c;
            c.name = cj.at("name").get<std::string>();
            c.match = cj.value("match", std::vector<std::string>{});
            c.file_count = cj.value("file_count", 0);
            if (!stats_from_json(cj.at("stats"), c.stats, err) ||
                !cov_from_json(cj.at("cov"), c.cov, err) || !validate_cov(c.stats, c.cov, err)) {
                return std::nullopt;
            }
            p.categories.push_back(std::move(c));
        }
    } catch (const std::exception &e) {
        err = e.what();
        return std::nullopt;
    }
    return p;
}

} // namespace sp
