#include <doctest/doctest.h>

#include <cstdint>
#include <ostream> // doctest stringifies std::string_view operands via operator<< (MSVC needs it)

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "soundpalette/metadata.h"

namespace fs = std::filesystem;

namespace {

std::string read_all(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("metadata/ixml_merge_and_parse") {
    sp::UcsMetadata m;
    m.cat_id = "GUNPis";
    m.category = "GUNS";
    m.sub_category = "PISTOL";
    m.fx_name = "Pistol Shot Dry";
    m.description = "A single dry pistol shot <with> a \"room\" tail & more.";
    m.keywords = {"pistol", "shot", "dry"};
    m.library = "sfx";

    // fresh document
    std::string doc = sp::merge_ixml_ucs("", m);
    CHECK(doc.find("<BWFXML>") != std::string::npos);
    CHECK(doc.find("<CATID>GUNPis</CATID>") != std::string::npos);
    CHECK(doc.find("&lt;with&gt;") != std::string::npos);
    sp::UcsMetadata back = sp::parse_ixml_ucs(doc);
    CHECK(back.cat_id == "GUNPis");
    CHECK(back.fx_name == "Pistol Shot Dry");
    CHECK(back.description == m.description); // entities round-trip
    CHECK(back.keywords == m.keywords);
    CHECK(back.library == "sfx");

    // merge into an existing document with a USER block and foreign fields
    std::string existing = "<?xml version=\"1.0\"?>\n<BWFXML><IXML_VERSION>1.5</IXML_VERSION>"
                           "<PROJECT>Game</PROJECT><USER><CATID>OLD</CATID><FOO>keep</FOO>"
                           "</USER></BWFXML>";
    std::string merged = sp::merge_ixml_ucs(existing, m);
    CHECK(merged.find("<PROJECT>Game</PROJECT>") != std::string::npos);
    CHECK(merged.find("<FOO>keep</FOO>") != std::string::npos);
    CHECK(merged.find("<CATID>OLD</CATID>") == std::string::npos);
    CHECK(merged.find("<CATID>GUNPis</CATID>") != std::string::npos);
    CHECK(sp::parse_ixml_ucs(merged).cat_id == "GUNPis");

    // document without a USER block gets one
    std::string no_user = "<BWFXML><PROJECT>Game</PROJECT></BWFXML>";
    std::string with_user = sp::merge_ixml_ucs(no_user, m);
    CHECK(with_user.find("<USER>") != std::string::npos);
    CHECK(sp::parse_ixml_ucs(with_user).sub_category == "PISTOL");

    // lowercase tags parse too; nothing found -> empty
    CHECK(sp::parse_ixml_ucs("<bwfxml><user><catid>AMBAir</catid></user></bwfxml>").cat_id ==
          "AMBAir");
    CHECK(sp::parse_ixml_ucs("<BWFXML/>").cat_id.empty());
}

TEST_CASE("metadata/wav_embed_roundtrip") {
    const fs::path src = "tests/golden/fixtures/click.wav";
    REQUIRE(fs::exists(src));
    const fs::path work = fs::temp_directory_path() / "sp_meta_click.wav";
    fs::copy_file(src, work, fs::copy_options::overwrite_existing);

    std::vector<sp::RiffChunk> before;
    std::string err;
    REQUIRE_MESSAGE(sp::read_wav_chunks(work, before, err), err);
    bool had_ixml = false;
    for (const sp::RiffChunk &c : before) {
        had_ixml = had_ixml || c.id == "iXML";
    }
    CHECK_FALSE(had_ixml);
    CHECK_FALSE(sp::read_wav_ucs_metadata(work, err).has_value());

    sp::UcsMetadata m;
    m.cat_id = "UIClick";
    m.category = "USER INTERFACE";
    m.sub_category = "CLICK";
    m.fx_name = "Soft Tap";
    m.description = "A soft confirm tap.";
    m.keywords = {"ui", "click"};
    sp::EmbedReport rep;
    REQUIRE_MESSAGE(sp::write_wav_ucs_metadata(work, m, true, rep, err), err);
    CHECK(rep.ixml_added);
    CHECK(rep.bext_added);
    CHECK(fs::exists(fs::path(work.string() + ".bak")));
    CHECK(read_all(fs::path(work.string() + ".bak")) == read_all(src));

    // every original chunk survives byte for byte, in order
    std::vector<sp::RiffChunk> after;
    REQUIRE(sp::read_wav_chunks(work, after, err));
    std::size_t j = 0;
    for (const sp::RiffChunk &c : before) {
        while (j < after.size() && after[j].id != c.id) {
            ++j;
        }
        REQUIRE(j < after.size());
        CHECK(after[j].data == c.data);
        ++j;
    }
    // the file still decodes (RIFF size right, data chunk intact)
    std::string bytes = read_all(work);
    std::uint32_t riff_size =
        static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[4])) |
        (static_cast<unsigned char>(bytes[5]) << 8) | (static_cast<unsigned char>(bytes[6]) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[7])) << 24);
    CHECK(riff_size + 8 == bytes.size());

    auto got = sp::read_wav_ucs_metadata(work, err);
    REQUIRE(got.has_value());
    CHECK(got->cat_id == "UIClick");
    CHECK(got->fx_name == "Soft Tap");
    CHECK(got->keywords == std::vector<std::string>{"ui", "click"});
    CHECK(got->bext_description == "Soft Tap - A soft confirm tap.");

    // second write updates in place: no duplicate chunks, same data hash
    m.description = "Changed.";
    sp::EmbedReport rep2;
    REQUIRE(sp::write_wav_ucs_metadata(work, m, false, rep2, err));
    CHECK(rep2.ixml_updated);
    CHECK(rep2.bext_updated);
    CHECK_FALSE(rep2.ixml_added);
    CHECK(rep2.data_sha256 == rep.data_sha256);
    std::vector<sp::RiffChunk> twice;
    REQUIRE(sp::read_wav_chunks(work, twice, err));
    int ixml_count = 0;
    for (const sp::RiffChunk &c : twice) {
        ixml_count += c.id == "iXML";
    }
    CHECK(ixml_count == 1);
    CHECK(sp::read_wav_ucs_metadata(work, err)->description == "Changed.");

    fs::remove(work);
    fs::remove(fs::path(work.string() + ".bak"));

    // non-WAV input is refused, never rewritten
    const fs::path ogg = fs::temp_directory_path() / "sp_meta.ogg";
    fs::copy_file("tests/golden/fixtures/sine440_1s.ogg", ogg,
                  fs::copy_options::overwrite_existing);
    std::string ogg_before = read_all(ogg);
    sp::EmbedReport rep3;
    CHECK_FALSE(sp::write_wav_ucs_metadata(ogg, m, false, rep3, err));
    CHECK(read_all(ogg) == ogg_before);
    fs::remove(ogg);
}
