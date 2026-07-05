#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "soundpalette/manifest.h"

namespace sp {

// 8x8 covariance over the mapping dims, row-major (extension-2 §4.1 shape, extension-3 v2
// dims). Diagonal == std².
using Cov8 = std::array<std::array<double, 8>, 8>;

struct CategoryProfile {
    std::string name;
    std::vector<std::string> match; // §4.2 glob patterns, first-match-wins across categories
    int file_count = 0;
    std::array<DimStats, 8> stats{};
    Cov8 cov{};
};

// A palette profile: named, portable identity statistics (extension-2 §4). Top-level stats
// and cov cover ALL contributing files; categories are optional refinements.
struct Profile {
    int profile_version = 1;
    int mapping_version = 2;
    double ref_spl = 75.0; // §4 convention the stats were computed under (extension-3 §0)
    std::string name;
    std::string description;
    double threshold = 2.5;
    std::string created_from_type = "folder"; // folder | manifest | selection
    std::string created_from_root;
    int created_from_file_count = 0;
    std::array<DimStats, 8> stats{};
    Cov8 cov{};
    std::vector<CategoryProfile> categories;
};

// §4.2 glob semantics: full-anchored, case-sensitive, forward slashes; ** spans segments,
// * = [^/]*, ? = one non-slash char. Hand-rolled regex translation, no third-party lib.
bool glob_match(std::string_view pattern, std::string_view path);

// First category (file order) with any matching pattern wins; -1 = top-level stats (§4.2).
int resolve_category(const Profile &profile, std::string_view path);

// Adapts any baseline manifest into an anonymous profile so lint/propose/harmonize consume
// Profile only; --baseline stays a compatibility alias (§4.3).
Profile profile_from_manifest(const Manifest &manifest);

// Builds a profile from analyzed entries (silent/error files never contribute, §4.1).
// category_specs: (name, patterns). Entries are assigned by first-match-wins.
Profile profile_from_entries(
    const std::vector<FileEntry> &entries, const std::string &name, const std::string &description,
    double threshold,
    const std::vector<std::pair<std::string, std::vector<std::string>>> &category_specs);

// Extension-3 §0 rule: artifacts must match the engine's mapping_version AND ref_spl;
// returns a human-readable refusal (telling the user to rescan/regenerate) or empty when
// compatible. Every comparison surface (lint, harmonize, GUI load) checks this first.
std::string profile_compat_error(const Profile &profile);

// Canonical JSON, 6-decimal floats (§4.1). Deterministic: same inputs -> identical bytes.
std::string profile_to_json(const Profile &profile);
std::optional<Profile> profile_from_json(const std::string &text, std::string &err);

} // namespace sp
