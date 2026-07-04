#pragma once

#include <string_view>

namespace sp {

// Gating seam (extension-2 §6.3): v3 always returns true, including for unknown names
// (fail-open until a license system exists). Guarded sites, by feature name:
//   "harmonize.apply"     CLI apply + harmonize non-dry-run
//   "mcp.write"           MCP propose/apply/harmonize/create_profile
//   "profile.create"      profile creation
//   "export.clean_sheet"  SVG export without the made-with mark (mark not implemented)
// Future licensing replaces this function body plus a key parser; nothing else moves.
bool capability(std::string_view feature);

} // namespace sp
