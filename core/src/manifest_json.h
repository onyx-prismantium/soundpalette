#pragma once

// In-core JSON helpers for FileEntry (extension-4 §5.2): the library stores each analysis in
// the manifest's canonical object shape (unrounded, so a later export re-rounds to bytes
// identical to a fresh scan). nlohmann is a private dependency of core, hence this header
// lives in src/, not include/.

#include <string>

#include <nlohmann/json.hpp>

#include "soundpalette/manifest.h"

namespace sp {

// Exactly the per-file object manifest_to_json emits when rounded == true; full-precision
// doubles otherwise.
nlohmann::ordered_json file_entry_to_json(const FileEntry &e, bool rounded);

// Inverse of the above (accepts either precision). Returns false and fills err on a missing
// or malformed field.
bool file_entry_from_json(const nlohmann::json &j, FileEntry &out, std::string &err);

} // namespace sp
