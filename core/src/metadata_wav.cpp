#include "soundpalette/metadata.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>

extern "C" {
#include "sha256.h"
}

namespace sp {

namespace {

std::string hex_lower(const unsigned char *bytes, std::size_t n) {
    static const char *kHex = "0123456789abcdef";
    std::string out(n * 2, '0');
    for (std::size_t i = 0; i < n; ++i) {
        out[2 * i] = kHex[(bytes[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[bytes[i] & 0xF];
    }
    return out;
}

std::string sha256_of(const std::string &data) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, reinterpret_cast<const unsigned char *>(data.data()), data.size());
    unsigned char digest[32];
    sha256_final(&ctx, digest);
    return hex_lower(digest, 32);
}

std::uint32_t le32(const char *p) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

void put_le32(std::string &out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

std::string xml_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(c);
        }
    }
    return out;
}

std::string xml_unescape(std::string s) {
    struct Pair {
        const char *from;
        const char *to;
    };
    static const Pair kPairs[] = {{"&lt;", "<"},   {"&gt;", ">"},  {"&quot;", "\""},
                                  {"&apos;", "'"}, {"&#39;", "'"}, {"&amp;", "&"}};
    for (const Pair &p : kPairs) {
        std::size_t pos = 0;
        std::string from = p.from;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), p.to);
            pos += std::strlen(p.to);
        }
    }
    return s;
}

std::string lower(std::string s) {
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Case-insensitive search for <tag> ... </tag>; returns the inner text (trimmed) or nullopt.
// `start`/`end` receive the span of the whole element when found.
std::optional<std::string> find_element(const std::string &doc, const std::string &tag,
                                        std::size_t from, std::size_t to, std::size_t *start,
                                        std::size_t *end) {
    std::string ldoc = lower(doc);
    std::string open = "<" + lower(tag);
    std::string close = "</" + lower(tag) + ">";
    std::size_t pos = from;
    while ((pos = ldoc.find(open, pos)) != std::string::npos && pos < to) {
        char after = pos + open.size() < ldoc.size() ? ldoc[pos + open.size()] : '\0';
        if (after != '>' && after != ' ' && after != '/') {
            pos += open.size();
            continue; // longer tag name with the same prefix
        }
        std::size_t gt = ldoc.find('>', pos);
        if (gt == std::string::npos) {
            return std::nullopt;
        }
        std::size_t cl = ldoc.find(close, gt);
        if (cl == std::string::npos || cl > to) {
            return std::nullopt;
        }
        std::string inner = doc.substr(gt + 1, cl - gt - 1);
        while (!inner.empty() && std::isspace(static_cast<unsigned char>(inner.front()))) {
            inner.erase(inner.begin());
        }
        while (!inner.empty() && std::isspace(static_cast<unsigned char>(inner.back()))) {
            inner.pop_back();
        }
        if (start) {
            *start = pos;
        }
        if (end) {
            *end = cl + close.size();
        }
        return xml_unescape(inner);
    }
    return std::nullopt;
}

struct Field {
    const char *tag;
    std::string UcsMetadata::*member;
};
const Field kFields[] = {
    {"CATID", &UcsMetadata::cat_id},
    {"CATEGORY", &UcsMetadata::category},
    {"SUBCATEGORY", &UcsMetadata::sub_category},
    {"FXNAME", &UcsMetadata::fx_name},
    {"DESCRIPTION", &UcsMetadata::description},
    {"LIBRARY", &UcsMetadata::library},
    {"CREATORID", &UcsMetadata::creator_id},
    {"SOURCEID", &UcsMetadata::source_id},
};

std::string keywords_joined(const std::vector<std::string> &kw) {
    std::string out;
    for (const std::string &k : kw) {
        out += (out.empty() ? "" : ", ") + k;
    }
    return out;
}

std::vector<std::string> keywords_split(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        while (!cur.empty() && cur.front() == ' ') {
            cur.erase(cur.begin());
        }
        while (!cur.empty() && cur.back() == ' ') {
            cur.pop_back();
        }
        if (!cur.empty()) {
            out.push_back(cur);
        }
        cur.clear();
    };
    for (char c : s) {
        if (c == ',' || c == ';') {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return out;
}

bool metadata_empty(const UcsMetadata &m) {
    return m.cat_id.empty() && m.category.empty() && m.sub_category.empty() && m.fx_name.empty() &&
           m.description.empty() && m.keywords.empty() && m.library.empty() &&
           m.creator_id.empty() && m.source_id.empty();
}

std::string bext_description_for(const UcsMetadata &meta) {
    std::string d = meta.fx_name;
    if (!meta.description.empty()) {
        d += (d.empty() ? "" : " - ") + meta.description;
    }
    if (d.size() > 256) {
        d.resize(256);
    }
    return d;
}

constexpr std::size_t kBextFixedSize = 602;

std::string bext_with_description(const std::string &existing, const std::string &description) {
    std::string b = existing;
    if (b.size() < kBextFixedSize) {
        std::string fresh(kBextFixedSize, '\0');
        // Originator (32 bytes at offset 256)
        std::string orig = "SoundPalette";
        std::memcpy(&fresh[256], orig.data(), orig.size());
        std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &now);
#else
        gmtime_r(&now, &tm);
#endif
        char date[16];
        char time_s[16];
        std::snprintf(date, sizeof date, "%04d-%02d-%02d", (tm.tm_year + 1900) % 10000,
                      (tm.tm_mon + 1) % 100, tm.tm_mday % 100);
        std::snprintf(time_s, sizeof time_s, "%02d:%02d:%02d", tm.tm_hour % 100, tm.tm_min % 100,
                      tm.tm_sec % 100);
        std::memcpy(&fresh[320], date, 10);
        std::memcpy(&fresh[330], time_s, 8);
        fresh[346] = 1; // Version 1 (UMID field present, zeroed)
        b = fresh;
    }
    std::memset(&b[0], 0, 256);
    std::memcpy(&b[0], description.data(), std::min<std::size_t>(256, description.size()));
    return b;
}

} // namespace

bool read_wav_chunks(const std::filesystem::path &path, std::vector<RiffChunk> &chunks,
                     std::string &err, bool skip_data) {
    chunks.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open " + path.string();
        return false;
    }
    char head[12];
    f.read(head, 12);
    if (f.gcount() != 12 || std::memcmp(head, "RIFF", 4) != 0 ||
        std::memcmp(head + 8, "WAVE", 4) != 0) {
        err = "not a RIFF/WAVE file";
        return false;
    }
    std::uint64_t pos = 12;
    for (;;) {
        char ch[8];
        f.read(ch, 8);
        if (f.gcount() != 8) {
            break;
        }
        RiffChunk c;
        c.id.assign(ch, 4);
        std::uint32_t size = le32(ch + 4);
        c.offset = pos;
        if (skip_data && c.id == "data") {
            f.seekg(static_cast<std::streamoff>(size + (size & 1)), std::ios::cur);
        } else {
            c.data.resize(size);
            if (size > 0) {
                f.read(&c.data[0], static_cast<std::streamsize>(size));
                std::streamsize got = f.gcount();
                if (got < static_cast<std::streamsize>(size)) {
                    c.data.resize(static_cast<std::size_t>(got)); // truncated file
                }
            }
            if (size & 1) {
                f.seekg(1, std::ios::cur);
            }
        }
        chunks.push_back(std::move(c));
        pos += 8 + size + (size & 1);
        if (!f) {
            break;
        }
    }
    return true;
}

UcsMetadata parse_ixml_ucs(const std::string &ixml) {
    UcsMetadata m;
    std::size_t ustart = 0;
    std::size_t uend = ixml.size();
    // Prefer the <USER> block; fall back to the whole document.
    std::size_t s = 0, e = 0;
    if (find_element(ixml, "USER", 0, ixml.size(), &s, &e)) {
        ustart = s;
        uend = e;
    }
    for (const Field &fld : kFields) {
        if (auto v = find_element(ixml, fld.tag, ustart, uend, nullptr, nullptr)) {
            m.*fld.member = *v;
        }
    }
    if (auto kw = find_element(ixml, "KEYWORDS", ustart, uend, nullptr, nullptr)) {
        m.keywords = keywords_split(*kw);
    }
    return m;
}

std::string merge_ixml_ucs(const std::string &existing, const UcsMetadata &meta) {
    std::string fields;
    auto add = [&](const char *tag, const std::string &value) {
        fields += "    <" + std::string(tag) + ">" + xml_escape(value) + "</" + tag + ">\n";
    };
    for (const Field &fld : kFields) {
        add(fld.tag, meta.*fld.member);
    }
    add("KEYWORDS", keywords_joined(meta.keywords));

    if (existing.empty()) {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<BWFXML>\n  <IXML_VERSION>1.61"
               "</IXML_VERSION>\n  <USER>\n" +
               fields + "  </USER>\n</BWFXML>\n";
    }
    std::string doc = existing;
    std::size_t us = 0, ue = 0;
    if (find_element(doc, "USER", 0, doc.size(), &us, &ue)) {
        // strip our fields from the block, then insert the fresh set before </USER>
        std::string block = doc.substr(us, ue - us);
        for (const Field &fld : kFields) {
            std::size_t fs = 0, fe = 0;
            while (find_element(block, fld.tag, 0, block.size(), &fs, &fe)) {
                block.erase(fs, fe - fs);
            }
        }
        std::size_t ks = 0, ke = 0;
        while (find_element(block, "KEYWORDS", 0, block.size(), &ks, &ke)) {
            block.erase(ks, ke - ks);
        }
        std::size_t close = lower(block).rfind("</user>");
        if (close == std::string::npos) {
            close = block.size();
        }
        block.insert(close, "\n" + fields);
        doc.replace(us, ue - us, block);
        return doc;
    }
    std::size_t root_close = lower(doc).rfind("</bwfxml>");
    std::string user_block = "  <USER>\n" + fields + "  </USER>\n";
    if (root_close == std::string::npos) {
        return doc + "\n" + user_block;
    }
    doc.insert(root_close, user_block);
    return doc;
}

std::optional<UcsMetadata> read_wav_ucs_metadata(const std::filesystem::path &path,
                                                 std::string &err) {
    std::vector<RiffChunk> chunks;
    if (!read_wav_chunks(path, chunks, err, true)) {
        return std::nullopt;
    }
    UcsMetadata m;
    std::string bext_description;
    bool any = false;
    for (const RiffChunk &c : chunks) {
        if (c.id == "iXML") {
            m = parse_ixml_ucs(c.data);
            any = !metadata_empty(m);
        } else if (c.id == "bext" && c.data.size() >= 256) {
            std::string d = c.data.substr(0, 256);
            d.erase(std::find(d.begin(), d.end(), '\0'), d.end());
            bext_description = d;
        }
    }
    m.bext_description = bext_description;
    if (!any) {
        return std::nullopt;
    }
    return m;
}

bool build_wav_with_ucs_metadata(const std::vector<RiffChunk> &chunks, const UcsMetadata &meta,
                                 std::string &out, EmbedReport &report, std::string &err) {
    std::vector<RiffChunk> work = chunks;
    int fmt_at = -1, data_at = -1, ixml_at = -1, bext_at = -1;
    for (std::size_t i = 0; i < work.size(); ++i) {
        if (work[i].id == "fmt ") {
            fmt_at = static_cast<int>(i);
        } else if (work[i].id == "data") {
            data_at = static_cast<int>(i);
        } else if (work[i].id == "iXML") {
            ixml_at = static_cast<int>(i);
        } else if (work[i].id == "bext") {
            bext_at = static_cast<int>(i);
        }
    }
    if (fmt_at < 0 || data_at < 0) {
        err = "WAV has no fmt/data chunk";
        return false;
    }
    report = EmbedReport{};
    report.data_sha256 = sha256_of(work[static_cast<std::size_t>(data_at)].data);

    // bext right after fmt (or updated in place), then iXML right after bext.
    std::string bext = bext_with_description(
        bext_at >= 0 ? work[static_cast<std::size_t>(bext_at)].data : std::string(),
        bext_description_for(meta));
    if (bext_at >= 0) {
        report.bext_updated = work[static_cast<std::size_t>(bext_at)].data != bext;
        work[static_cast<std::size_t>(bext_at)].data = bext;
    } else {
        RiffChunk c;
        c.id = "bext";
        c.data = bext;
        work.insert(work.begin() + fmt_at + 1, c);
        bext_at = fmt_at + 1;
        if (ixml_at > fmt_at) {
            ++ixml_at;
        }
        report.bext_added = true;
    }
    std::string ixml = merge_ixml_ucs(
        ixml_at >= 0 ? work[static_cast<std::size_t>(ixml_at)].data : std::string(), meta);
    if (ixml_at >= 0) {
        report.ixml_updated = work[static_cast<std::size_t>(ixml_at)].data != ixml;
        work[static_cast<std::size_t>(ixml_at)].data = ixml;
    } else {
        RiffChunk c;
        c.id = "iXML";
        c.data = ixml;
        work.insert(work.begin() + bext_at + 1, c);
        report.ixml_added = true;
    }

    std::string body = "WAVE";
    for (const RiffChunk &c : work) {
        body += c.id;
        put_le32(body, static_cast<std::uint32_t>(c.data.size()));
        body += c.data;
        if (c.data.size() & 1) {
            body.push_back('\0');
        }
    }
    out.clear();
    out += "RIFF";
    put_le32(out, static_cast<std::uint32_t>(body.size()));
    out += body;
    return true;
}

bool write_wav_ucs_metadata(const std::filesystem::path &path, const UcsMetadata &meta, bool backup,
                            EmbedReport &report, std::string &err) {
    std::vector<RiffChunk> chunks;
    if (!read_wav_chunks(path, chunks, err)) {
        return false;
    }
    std::string image;
    if (!build_wav_with_ucs_metadata(chunks, meta, image, report, err)) {
        return false;
    }
    std::error_code ec;
    if (backup) {
        std::filesystem::path bak = path;
        bak += ".bak";
        std::filesystem::copy_file(path, bak, std::filesystem::copy_options::overwrite_existing,
                                   ec);
        if (ec) {
            err = "cannot write backup " + bak.string();
            return false;
        }
    }
    std::filesystem::path tmp = path;
    tmp += ".sp-tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) {
            err = "cannot write " + tmp.string();
            return false;
        }
        f.write(image.data(), static_cast<std::streamsize>(image.size()));
        if (!f) {
            err = "write failed for " + tmp.string();
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        err = "cannot replace " + path.string();
        return false;
    }
    // verify: the audio payload must be byte-identical
    std::vector<RiffChunk> after;
    if (!read_wav_chunks(path, after, err)) {
        return false;
    }
    for (const RiffChunk &c : after) {
        if (c.id == "data") {
            if (sha256_of(c.data) != report.data_sha256) {
                err = "data chunk changed after rewrite (this should never happen)";
                return false;
            }
            return true;
        }
    }
    err = "data chunk missing after rewrite";
    return false;
}

} // namespace sp
