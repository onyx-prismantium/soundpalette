#pragma once

// Extension-4 §10: UCS metadata inside WAV files. Read: the iXML <USER> fields tools like
// Soundminer, BaseHead and Reaper's media explorer exchange, so a file that already carries
// UCS tags is annotated with source "metadata" on ingest. Write (opt-in `library embed`):
// rewrite only the iXML and bext chunks, keep every other chunk byte for byte, verify the
// audio data chunk hash before and after. WAV only; other containers are refused.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sp {

struct UcsMetadata {
    std::string cat_id;
    std::string category;
    std::string sub_category;
    std::string fx_name;
    std::string description;
    std::vector<std::string> keywords;
    std::string library;
    std::string creator_id;
    std::string source_id;
    // bext Description (256 chars max) as found, informational
    std::string bext_description;
};

// One RIFF chunk as read from disk (payload excludes the 8-byte header and the pad byte).
struct RiffChunk {
    std::string id;           // 4 chars
    std::string data;         // payload bytes
    std::uint64_t offset = 0; // file offset of the chunk header
};

// Parses the top-level chunk list of a RIFF/WAVE file. false + err if not a WAVE. With
// skip_data, the audio payload is not loaded (data.empty(), header only) — the cheap path for
// reading metadata during ingest.
bool read_wav_chunks(const std::filesystem::path &path, std::vector<RiffChunk> &chunks,
                     std::string &err, bool skip_data = false);

// Reads UCS fields from the iXML and bext chunks. nullopt when the file is not a WAV or
// carries none of the fields (an empty struct is never returned).
std::optional<UcsMetadata> read_wav_ucs_metadata(const std::filesystem::path &path,
                                                 std::string &err);

// Rewrites `path` with the iXML <USER> UCS fields and the bext Description
// ("FXName - description") set from `meta`; other chunks are preserved in order. Existing iXML
// content outside the UCS fields is kept. With `backup`, the original is copied to
// "<name>.bak" first. Returns false + err on any failure; on success the data chunk is
// verified byte-identical (sha256) and `report` says what changed.
struct EmbedReport {
    bool ixml_added = false;
    bool ixml_updated = false;
    bool bext_added = false;
    bool bext_updated = false;
    std::string data_sha256; // of the audio payload, identical before and after
};
bool write_wav_ucs_metadata(const std::filesystem::path &path, const UcsMetadata &meta, bool backup,
                            EmbedReport &report, std::string &err);

// Produces the bytes `write_wav_ucs_metadata` would write, without touching disk (dry runs and
// tests). `out` receives the new file image.
bool build_wav_with_ucs_metadata(const std::vector<RiffChunk> &chunks, const UcsMetadata &meta,
                                 std::string &out, EmbedReport &report, std::string &err);

// The iXML document with the UCS <USER> fields merged into `existing_ixml` (may be empty).
std::string merge_ixml_ucs(const std::string &existing_ixml, const UcsMetadata &meta);

// Parses UCS fields out of an iXML document; empty struct if none.
UcsMetadata parse_ixml_ucs(const std::string &ixml);

} // namespace sp
