#include <doctest/doctest.h>

#include <cstdint>
#include <ostream> // doctest stringifies std::string_view operands via operator<< (MSVC needs it)

#include <algorithm>
#include <string>
#include <vector>

#include "soundpalette/ucs.h"

TEST_CASE("ucs/table") {
    // Extension-4 §4.1: the vendored v8.2.1 list, verbatim.
    CHECK(std::string(sp::ucs_version()) == "8.2.1");
    CHECK(sp::ucs_entries().size() == 753);
    CHECK(sp::ucs_categories().size() == 82);
    CHECK(sp::ucs_categories().front() == "AIR");

    const sp::UcsEntry *e = sp::ucs_find("GUNAuto");
    REQUIRE(e != nullptr);
    CHECK(e->category == "GUNS");
    CHECK(e->sub_category == "AUTOMATIC");
    CHECK(e->cat_short == "GUN");
    CHECK_FALSE(e->explanation.empty());
    CHECK_FALSE(e->synonyms.empty());
    CHECK(sp::ucs_find("gunauto") == nullptr); // case-sensitive
    CHECK(sp::ucs_find("") == nullptr);

    CHECK(sp::ucs_in_category("AMBIENCE").size() == 47);
    CHECK(sp::ucs_in_category("NOPE").empty());

    // Every CatID unique and every entry's category listed.
    std::vector<std::string> ids;
    for (const sp::UcsEntry &x : sp::ucs_entries()) {
        ids.emplace_back(x.cat_id);
        bool listed = false;
        for (std::string_view c : sp::ucs_categories()) {
            listed = listed || c == x.category;
        }
        CHECK(listed);
    }
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

TEST_CASE("ucs/tokenize") {
    using V = std::vector<std::string>;
    CHECK(sp::ucs_tokenize("sword_swing_whoosh_01") == V{"sword", "swing", "whoosh"});
    CHECK(sp::ucs_tokenize("MenuClickConfirm") == V{"menu", "click", "confirm"});
    CHECK(sp::ucs_tokenize("Explosions, large; Glasses!") == V{"explosion", "large", "glass"});
    CHECK(sp::ucs_tokenize("crashes boxes") == V{"crash", "box"});
    CHECK(sp::ucs_tokenize("bass 808 x") == V{"bass"}); // digit-only and 1-char tokens dropped
    CHECK(sp::ucs_tokenize("Caf\xC3\xA9 Stra\xC3\x9F"
                           "e") == V{"cafe", "strase"}); // accents folded
    CHECK(sp::ucs_tokenize("") == V{});
}

TEST_CASE("ucs/rank") {
    // §4.2 deterministic scoring: SubCategory > Category > Synonym > Explanation.
    auto top = [](std::vector<std::string> tokens, std::string_view cat = {}) {
        std::vector<sp::UcsMatch> r = sp::ucs_rank(tokens, cat);
        return r.empty() ? std::string() : std::string(r.front().entry->cat_id);
    };
    CHECK(top({"gun", "pistol", "shot"}) == "GUNPis");
    CHECK(top({"magic", "spell", "cast"}) == "MAGSpel");
    CHECK(top({"water", "splash", "small"}) == "WATRSplsh");
    CHECK(top({"door", "creak"}) == "DOORCreak");
    CHECK(top({"pistol"}, "GUNS") == "GUNPis");
    CHECK(top({"pistol"}, "AMBIENCE") != "GUNPis");
    CHECK(sp::ucs_rank(std::vector<std::string>{"qzxv"}).empty());

    // Scores: a SubCategory word alone scores kSubCategoryScore, duplicates count once.
    std::vector<sp::UcsMatch> r =
        sp::ucs_rank(std::vector<std::string>{"pistol", "pistol"}, "GUNS");
    REQUIRE_FALSE(r.empty());
    CHECK(r.front().entry->cat_id == "GUNPis");
    CHECK(r.front().score == sp::kSubCategoryScore);
    // Sorted descending, stable in CSV order for ties.
    for (std::size_t k = 1; k < r.size(); ++k) {
        CHECK(r[k - 1].score >= r[k].score);
    }
}

TEST_CASE("ucs/filename") {
    auto f = sp::ucs_parse_filename("GUNAuto_Uzi Bursts_TN_DORY.wav");
    REQUIRE(f.has_value());
    CHECK(f->cat_id == "GUNAuto");
    CHECK(f->fx_name == "Uzi Bursts");
    CHECK(f->creator_id == "TN");
    CHECK(f->source_id == "DORY");
    CHECK(f->user_data.empty());

    auto g = sp::ucs_parse_filename("dir/sub/AMBForst_Night Crickets_AS_LIB_take2_v3.flac");
    REQUIRE(g.has_value());
    CHECK(g->cat_id == "AMBForst");
    CHECK(g->user_data == "take2_v3");

    auto minimal = sp::ucs_parse_filename("UIClick_Soft Tap");
    REQUIRE(minimal.has_value());
    CHECK(minimal->creator_id.empty());

    CHECK_FALSE(sp::ucs_parse_filename("sword_swing.wav").has_value()); // not a CatID
    CHECK_FALSE(sp::ucs_parse_filename("GUNAuto.wav").has_value());     // no FXName
    CHECK_FALSE(sp::ucs_parse_filename("GUNAuto_ .wav").has_value());   // empty FXName
    CHECK_FALSE(sp::ucs_parse_filename("gunauto_x.wav").has_value());   // case-sensitive

    sp::UcsFilename c;
    c.cat_id = "GUNAuto";
    c.fx_name = "Uzi Bursts";
    c.creator_id = "TN";
    CHECK(sp::ucs_compose_stem(c) == "GUNAuto_Uzi Bursts_TN");
    c.creator_id.clear();
    CHECK(sp::ucs_compose_stem(c) == "GUNAuto_Uzi Bursts");
    c.source_id = "DORY";
    CHECK(sp::ucs_compose_stem(c) == "GUNAuto_Uzi Bursts__DORY");
}
