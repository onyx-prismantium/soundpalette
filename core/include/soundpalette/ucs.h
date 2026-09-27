#pragma once

// Universal Category System (UCS) taxonomy — extension-4 §4. The list is vendored verbatim
// (assets/ucs/, public domain) and compiled in via the generated core/src/ucs_data.cpp, so the
// core needs no file at runtime. All views point into static storage and never dangle.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sp {

struct UcsEntry {
    std::string_view category;     // e.g. "GUNS"
    std::string_view sub_category; // e.g. "AUTOMATIC"
    std::string_view cat_id;       // e.g. "GUNAuto" (unique, case-sensitive)
    std::string_view cat_short;    // e.g. "GUN"
    std::string_view explanation;
    std::vector<std::string_view> synonyms; // the CSV's comma-separated list, split + trimmed
};

// "8.2.1"
const char *ucs_version();

// All entries in CSV order (753 for v8.2.1).
const std::vector<UcsEntry> &ucs_entries();

// Exact, case-sensitive CatID lookup; nullptr if unknown.
const UcsEntry *ucs_find(std::string_view cat_id);

// The distinct top-level categories in CSV order (82 for v8.2.1).
const std::vector<std::string_view> &ucs_categories();

// Entries of one category, CSV order; empty for an unknown category.
std::vector<const UcsEntry *> ucs_in_category(std::string_view category);

// Search tokenizer shared by ranking, the offline classifier and FTS query building:
// lowercase, Latin-1 accents folded to ASCII, split on non-alphanumerics, camelCase and
// letter/digit boundaries; light plural folding ("explosions" -> "explosion"); pure-digit
// tokens and tokens shorter than 2 chars are dropped.
std::vector<std::string> ucs_tokenize(std::string_view text);

struct UcsMatch {
    const UcsEntry *entry = nullptr;
    int score = 0;
};

// Deterministic keyword ranking (§4.2): for every distinct query token an entry earns the best
// of kSubCategoryScore (token is a word of its SubCategory), kCategoryScore (word of its
// Category), kSynonymScore (word of one of its Synonyms) or kExplanationScore (appears in its
// Explanations); scores are summed. Entries scoring 0 are omitted. Sorted by score
// descending, ties in CSV order. `category` restricts to one category.
inline constexpr int kSubCategoryScore = 5;
inline constexpr int kCategoryScore = 5;
inline constexpr int kSynonymScore = 2;
inline constexpr int kExplanationScore = 1;
std::vector<UcsMatch> ucs_rank(std::span<const std::string> tokens, std::string_view category = {});

// Same, with a second token list whose hits are capped at kSynonymScore (folder names are
// weaker evidence than the filename: a "foley/" folder must not outvote the file's own words).
std::vector<UcsMatch> ucs_rank_weak(std::span<const std::string> tokens,
                                    std::span<const std::string> weak_tokens,
                                    std::string_view category = {});

// UCS filename grammar: CatID_FXName_CreatorID_SourceID[_UserData].ext (underscores separate
// fields; FXName may contain spaces and hyphens). Format-only: CatID must exist in the list,
// FXName must be non-empty; CreatorID/SourceID/UserData are optional. Never guesses.
struct UcsFilename {
    std::string cat_id;
    std::string fx_name;
    std::string creator_id;
    std::string source_id;
    std::string user_data;
};
std::optional<UcsFilename> ucs_parse_filename(std::string_view filename);

// Composes "CatID_FXName_CreatorID_SourceID[_UserData]" (no extension). Empty trailing fields
// are omitted; an empty field before a non-empty one is emitted as-is (caller validates).
std::string ucs_compose_stem(const UcsFilename &fields);

} // namespace sp
