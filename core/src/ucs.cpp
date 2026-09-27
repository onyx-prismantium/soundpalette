#include "soundpalette/ucs.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace sp {

namespace ucs_data {
extern const char *const kVersion;
extern const unsigned kRowCount;
extern const char *const kRows[][6];
} // namespace ucs_data

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

// Latin-1 supplement (UTF-8 lead byte 0xC3) folded to a plain ASCII letter, or 0 if none.
char fold_c3(unsigned char second) {
    static const char *const kMap = // 0x80..0xBF after 0xC3: À..ÿ
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (second < 0x80 || second > 0xBF) {
        return 0;
    }
    char c = kMap[second - 0x80];
    if (c == 'x' || c == '/') {
        return 0;
    }
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool is_digit(char c) {
    return c >= '0' && c <= '9';
}
bool is_upper(char c) {
    return c >= 'A' && c <= 'Z';
}
bool is_lower(char c) {
    return c >= 'a' && c <= 'z';
}

struct Index {
    std::vector<UcsEntry> entries;
    std::vector<std::string_view> categories;
    std::unordered_map<std::string_view, std::size_t> by_id;
    // Per-entry token sets for ranking (§4.2), built once.
    std::vector<std::unordered_set<std::string>> cat_tokens;
    std::vector<std::unordered_set<std::string>> sub_tokens;
    std::vector<std::unordered_set<std::string>> syn_tokens;
    std::vector<std::unordered_set<std::string>> expl_tokens;
};

const Index &index() {
    static const Index idx = [] {
        Index i;
        i.entries.reserve(ucs_data::kRowCount);
        for (unsigned r = 0; r < ucs_data::kRowCount; ++r) {
            const char *const *row = ucs_data::kRows[r];
            UcsEntry e;
            e.category = row[0];
            e.sub_category = row[1];
            e.cat_id = row[2];
            e.cat_short = row[3];
            e.explanation = row[4];
            std::string_view syn = row[5];
            while (!syn.empty()) {
                std::size_t comma = syn.find(',');
                std::string_view part = trim(syn.substr(0, comma));
                if (!part.empty()) {
                    e.synonyms.push_back(part);
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                syn.remove_prefix(comma + 1);
            }
            i.by_id.emplace(e.cat_id, i.entries.size());
            if (i.categories.empty() || i.categories.back() != e.category) {
                if (std::find(i.categories.begin(), i.categories.end(), e.category) ==
                    i.categories.end()) {
                    i.categories.push_back(e.category);
                }
            }
            i.entries.push_back(std::move(e));
        }
        i.cat_tokens.resize(i.entries.size());
        i.sub_tokens.resize(i.entries.size());
        i.syn_tokens.resize(i.entries.size());
        i.expl_tokens.resize(i.entries.size());
        for (std::size_t k = 0; k < i.entries.size(); ++k) {
            const UcsEntry &e = i.entries[k];
            for (const std::string &t : ucs_tokenize(e.category)) {
                i.cat_tokens[k].insert(t);
            }
            for (const std::string &t : ucs_tokenize(e.sub_category)) {
                i.sub_tokens[k].insert(t);
            }
            for (std::string_view s : e.synonyms) {
                for (const std::string &t : ucs_tokenize(s)) {
                    i.syn_tokens[k].insert(t);
                }
            }
            for (const std::string &t : ucs_tokenize(e.explanation)) {
                i.expl_tokens[k].insert(t);
            }
        }
        return i;
    }();
    return idx;
}

} // namespace

const char *ucs_version() {
    return ucs_data::kVersion;
}

const std::vector<UcsEntry> &ucs_entries() {
    return index().entries;
}

const UcsEntry *ucs_find(std::string_view cat_id) {
    const Index &i = index();
    auto it = i.by_id.find(cat_id);
    return it == i.by_id.end() ? nullptr : &i.entries[it->second];
}

const std::vector<std::string_view> &ucs_categories() {
    return index().categories;
}

std::vector<const UcsEntry *> ucs_in_category(std::string_view category) {
    std::vector<const UcsEntry *> out;
    for (const UcsEntry &e : index().entries) {
        if (e.category == category) {
            out.push_back(&e);
        }
    }
    return out;
}

std::vector<std::string> ucs_tokenize(std::string_view text) {
    // Pass 1: fold to a lowercase ASCII stream with explicit boundaries ('\0' separators).
    std::string folded;
    folded.reserve(text.size());
    char prev = 0;
    for (std::size_t p = 0; p < text.size(); ++p) {
        unsigned char c = static_cast<unsigned char>(text[p]);
        char out = 0;
        if (c == 0xC3 && p + 1 < text.size()) {
            out = fold_c3(static_cast<unsigned char>(text[p + 1]));
            ++p;
            if (out == 0) {
                // unknown accented char: treat as a boundary
                folded.push_back('\0');
                prev = 0;
                continue;
            }
            if (prev != 0 && is_digit(prev)) {
                folded.push_back('\0');
            }
            folded.push_back(out);
            prev = out;
            continue;
        }
        if (c >= 0x80) {
            // other multi-byte sequences: boundary (skip continuation bytes)
            folded.push_back('\0');
            prev = 0;
            continue;
        }
        char ch = static_cast<char>(c);
        if (is_alpha(ch)) {
            bool boundary = (prev != 0 && is_digit(prev)) || (is_lower(prev) && is_upper(ch));
            if (boundary) {
                folded.push_back('\0');
            }
            folded.push_back(static_cast<char>(std::tolower(c)));
            prev = ch;
        } else if (is_digit(ch)) {
            if (prev != 0 && is_alpha(prev)) {
                folded.push_back('\0');
            }
            folded.push_back(ch);
            prev = ch;
        } else {
            folded.push_back('\0');
            prev = 0;
        }
    }
    // Pass 2: split, drop pure-digit and short tokens.
    std::vector<std::string> tokens;
    std::string cur;
    auto flush = [&] {
        if (cur.size() >= 2 && !std::all_of(cur.begin(), cur.end(), is_digit)) {
            // light plural folding: "explosions" -> "explosion", "glasses" -> "glass",
            // "houses" -> "house", "batteries" -> "battery"
            if (cur.size() > 4 && cur.ends_with("ies")) {
                cur.resize(cur.size() - 3);
                cur.push_back('y');
            } else if (cur.size() > 4 && cur.ends_with("es")) {
                cur.resize(cur.size() - 2);
                if (!(cur.ends_with("ch") || cur.ends_with("sh") || cur.ends_with("x") ||
                      cur.ends_with("ss") || cur.ends_with("zz"))) {
                    cur.push_back('e');
                }
            } else if (cur.size() > 3 && cur.back() == 's' && !cur.ends_with("ss") &&
                       !cur.ends_with("us") && !cur.ends_with("is")) {
                cur.pop_back();
            }
            tokens.push_back(cur);
        }
        cur.clear();
    };
    for (char ch : folded) {
        if (ch == '\0') {
            flush();
        } else {
            cur.push_back(ch);
        }
    }
    flush();
    return tokens;
}

namespace {

int token_score(const Index &i, std::size_t k, const std::string &t) {
    if (i.sub_tokens[k].count(t)) {
        return kSubCategoryScore;
    }
    if (i.cat_tokens[k].count(t)) {
        return kCategoryScore;
    }
    if (i.syn_tokens[k].count(t)) {
        return kSynonymScore;
    }
    if (i.expl_tokens[k].count(t)) {
        return kExplanationScore;
    }
    return 0;
}

std::vector<std::string> distinct_tokens(std::span<const std::string> tokens) {
    std::vector<std::string> out;
    for (const std::string &t : tokens) {
        if (std::find(out.begin(), out.end(), t) == out.end()) {
            out.push_back(t);
        }
    }
    return out;
}

} // namespace

std::vector<UcsMatch> ucs_rank_weak(std::span<const std::string> tokens,
                                    std::span<const std::string> weak_tokens,
                                    std::string_view category) {
    const Index &i = index();
    std::vector<std::string> strong = distinct_tokens(tokens);
    std::vector<std::string> weak;
    for (const std::string &t : distinct_tokens(weak_tokens)) {
        if (std::find(strong.begin(), strong.end(), t) == strong.end()) {
            weak.push_back(t);
        }
    }
    std::vector<UcsMatch> out;
    for (std::size_t k = 0; k < i.entries.size(); ++k) {
        const UcsEntry &e = i.entries[k];
        if (!category.empty() && e.category != category) {
            continue;
        }
        int score = 0;
        for (const std::string &t : strong) {
            score += token_score(i, k, t);
        }
        for (const std::string &t : weak) {
            score += std::min(token_score(i, k, t), kSynonymScore);
        }
        if (score > 0) {
            out.push_back(UcsMatch{&e, score});
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const UcsMatch &a, const UcsMatch &b) { return a.score > b.score; });
    return out;
}

std::vector<UcsMatch> ucs_rank(std::span<const std::string> tokens, std::string_view category) {
    return ucs_rank_weak(tokens, {}, category);
}

std::optional<UcsFilename> ucs_parse_filename(std::string_view filename) {
    // strip directory and extension
    std::size_t slash = filename.find_last_of("/\\");
    if (slash != std::string_view::npos) {
        filename.remove_prefix(slash + 1);
    }
    std::size_t dot = filename.rfind('.');
    if (dot != std::string_view::npos && dot > 0) {
        std::string_view ext = filename.substr(dot + 1);
        bool ext_ok =
            ext.size() >= 2 && ext.size() <= 4 &&
            std::all_of(ext.begin(), ext.end(), [](char c) { return is_alpha(c) || is_digit(c); });
        if (ext_ok) {
            filename = filename.substr(0, dot);
        }
    }
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        std::size_t us = filename.find('_', start);
        fields.push_back(filename.substr(start, us == std::string_view::npos ? us : us - start));
        if (us == std::string_view::npos) {
            break;
        }
        start = us + 1;
    }
    if (fields.size() < 2) {
        return std::nullopt;
    }
    if (ucs_find(fields[0]) == nullptr) {
        return std::nullopt;
    }
    std::string_view fx = trim(fields[1]);
    if (fx.empty()) {
        return std::nullopt;
    }
    UcsFilename f;
    f.cat_id = std::string(fields[0]);
    f.fx_name = std::string(fx);
    if (fields.size() > 2) {
        f.creator_id = std::string(trim(fields[2]));
    }
    if (fields.size() > 3) {
        f.source_id = std::string(trim(fields[3]));
    }
    for (std::size_t k = 4; k < fields.size(); ++k) {
        if (!f.user_data.empty()) {
            f.user_data += '_';
        }
        f.user_data += std::string(fields[k]);
    }
    return f;
}

std::string ucs_compose_stem(const UcsFilename &fields) {
    std::vector<std::string> parts = {fields.cat_id, fields.fx_name, fields.creator_id,
                                      fields.source_id, fields.user_data};
    while (parts.size() > 2 && parts.back().empty()) {
        parts.pop_back();
    }
    std::string out;
    for (std::size_t k = 0; k < parts.size(); ++k) {
        if (k > 0) {
            out += '_';
        }
        out += parts[k];
    }
    return out;
}

} // namespace sp
