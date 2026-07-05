#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "soundpalette/profile.h"

namespace sp {

// Built-in genre presets: hand-designed palette profiles (created_from.type == "designed").
// Their statistics are authored priors, not corpus measurements — generous stds, diagonal
// covariance (no correlation claimed), default threshold. They act as genre guardrails; a
// derived (corpus-measured) profile with the same name can replace one at any time because
// presets are ordinary Profiles end to end.
struct PresetInfo {
    std::string slug; // CLI/menu key, e.g. "sci-fi"
    std::string name; // display + Profile::name, e.g. "Sci-Fi"
    std::string description;
};

const std::vector<PresetInfo> &builtin_preset_list();

// Preset by slug (exact match); nullopt for unknown slugs.
std::optional<Profile> builtin_preset(std::string_view slug);

} // namespace sp
