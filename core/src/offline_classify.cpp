// Extension-4 §8: model-free classification from what the path already says. Never invents a
// CatID from acoustic features (brightness does not know a sword from a spoon).

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "soundpalette/library.h"
#include "soundpalette/ucs.h"

namespace sp {

namespace {

const std::unordered_set<std::string> &stopwords() {
    static const std::unordered_set<std::string> kWords = {
        "wav",     "mp3",   "ogg",    "flac", "aif", "aiff", "sfx",     "sound", "sounds",
        "fx",      "audio", "final",  "new",  "old", "copy", "mix",     "take",  "var",
        "version", "ver",   "stereo", "mono", "the", "and",  "of",      "for",   "with",
        "to",      "in",    "on",     "at",   "by",  "lib",  "library", "pack",  "master",
        "edit",    "raw",   "orig",   "temp", "tmp", "test", "untitled"};
    return kWords;
}

std::vector<std::string> filtered_tokens(std::string_view text) {
    std::vector<std::string> out;
    for (std::string &t : ucs_tokenize(text)) {
        if (!stopwords().count(t) && std::find(out.begin(), out.end(), t) == out.end()) {
            out.push_back(std::move(t));
        }
    }
    return out;
}

// Raw words of a stem for the FX name: split on separators, camelCase and digits, keep the
// spelling (no plural folding, unlike ucs_tokenize), drop stopwords and pure numbers.
std::string title_case_words(std::string_view stem) {
    std::vector<std::string> words;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty() &&
            !std::all_of(cur.begin(), cur.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            std::string low = cur;
            for (char &ch : low) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            if (!stopwords().count(low)) {
                low[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(low[0])));
                words.push_back(low);
            }
        }
        cur.clear();
    };
    char prev = 0;
    for (char ch : stem) {
        bool alpha = std::isalpha(static_cast<unsigned char>(ch)) != 0;
        bool digit = std::isdigit(static_cast<unsigned char>(ch)) != 0;
        if (!alpha && !digit) {
            flush();
            prev = 0;
            continue;
        }
        bool camel = std::islower(static_cast<unsigned char>(prev)) &&
                     std::isupper(static_cast<unsigned char>(ch));
        bool kind_change =
            (std::isdigit(static_cast<unsigned char>(prev)) != 0) != digit && prev != 0;
        if (camel || kind_change) {
            flush();
        }
        cur.push_back(ch);
        prev = ch;
    }
    flush();
    std::string out;
    for (const std::string &w : words) {
        out += (out.empty() ? "" : " ") + w;
    }
    return out;
}

} // namespace

std::optional<Annotation> classify_offline_path(std::string_view rel_path) {
    std::string path(rel_path);
    std::size_t slash = path.find_last_of('/');
    std::string filename = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string folders = slash == std::string::npos ? std::string() : path.substr(0, slash);
    std::string stem = filename;
    if (std::size_t dot = stem.rfind('.'); dot != std::string::npos && dot > 0) {
        stem = stem.substr(0, dot);
    }

    // 1. The file already speaks UCS.
    if (std::optional<UcsFilename> f = ucs_parse_filename(filename)) {
        Annotation a;
        a.cat_id = f->cat_id;
        a.fx_name = f->fx_name;
        a.keywords = filtered_tokens(f->fx_name);
        a.confidence = 1.0;
        a.source = AnnotationSource::kFilename;
        return a;
    }

    // 2. Token match over stem + folder names.
    std::vector<std::string> stem_tokens = filtered_tokens(stem);
    std::vector<std::string> folder_tokens = filtered_tokens(folders);
    std::vector<std::string> all_tokens = stem_tokens;
    for (std::string &t : folder_tokens) {
        if (std::find(all_tokens.begin(), all_tokens.end(), t) == all_tokens.end()) {
            all_tokens.push_back(t);
        }
    }
    if (all_tokens.empty()) {
        return std::nullopt;
    }
    // Folder names are weak evidence (capped at synonym weight), the filename is strong.
    std::vector<UcsMatch> ranked = ucs_rank_weak(stem_tokens, folder_tokens);
    if (ranked.empty()) {
        return std::nullopt;
    }
    // Accept only an unambiguous winner that carries at least one Category/SubCategory word
    // (or three synonym hits): a tie means the name alone cannot decide, so stay honest.
    int top = ranked[0].score;
    int runner = ranked.size() > 1 ? ranked[1].score : 0;
    if (top < kSubCategoryScore || top <= runner) {
        return std::nullopt;
    }
    Annotation a;
    a.cat_id = std::string(ranked[0].entry->cat_id);
    a.fx_name = title_case_words(stem);
    if (a.fx_name.empty()) {
        a.fx_name = title_case_words(folders);
    }
    a.keywords = all_tokens;
    a.confidence = 0.4;
    // Source is "filename" when the stem alone already points at this entry, else "folder".
    a.source = AnnotationSource::kFolder;
    if (!stem_tokens.empty()) {
        std::vector<UcsMatch> stem_only = ucs_rank(stem_tokens);
        if (!stem_only.empty() && stem_only[0].entry == ranked[0].entry) {
            a.source = AnnotationSource::kFilename;
        }
    }
    return a;
}

} // namespace sp
