#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "soundpalette/library.h"
#include "soundpalette/manifest.h"

namespace fs = std::filesystem;

namespace {

// A temp library root populated from the golden fixtures (copied, so mtimes/paths are ours).
struct TempRoot {
    fs::path root;
    TempRoot() {
        root = fs::temp_directory_path() /
               ("sp_lib_" + std::to_string(static_cast<long long>(std::time(nullptr))) + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this) & 0xffff));
        fs::create_directories(root / "ui");
        fs::create_directories(root / "combat");
        const fs::path fx = fs::path("tests/golden/fixtures");
        fs::copy_file(fx / "click.wav", root / "ui" / "UIClick_Soft Tap_SP_TEST.wav");
        fs::copy_file(fx / "click.wav", root / "ui" / "menu_click_confirm.wav");
        fs::copy_file(fx / "bright_outlier.wav", root / "combat" / "gun_pistol_shot_01.wav");
        fs::copy_file(fx / "sine440_1s.wav", root / "tone.wav");
    }
    ~TempRoot() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

} // namespace

TEST_CASE("library/create_open_nesting") {
    TempRoot t;
    std::string err;
    CHECK(sp::Library::open(t.root, err) == nullptr); // nothing yet
    auto lib = sp::Library::create(t.root, err);
    REQUIRE_MESSAGE(lib != nullptr, err);
    CHECK(fs::exists(sp::Library::index_path(t.root)));
    CHECK(lib->meta("schema_version") == "1");
    CHECK(lib->meta("ucs_version") == "8.2.1");
    // roots never nest (§5.1)
    CHECK(sp::Library::create(t.root / "ui", err) == nullptr);
    CHECK(err.find("already exists") != std::string::npos);
    // find_root walks up from a file
    auto found = sp::Library::find_root(t.root / "ui" / "menu_click_confirm.wav");
    REQUIRE(found.has_value());
    CHECK(fs::equivalent(*found, t.root));
}

TEST_CASE("library/update_incremental_and_export") {
    TempRoot t;
    std::string err;
    auto lib = sp::Library::create(t.root, err);
    REQUIRE(lib != nullptr);
    sp::UpdateOptions opt;
    opt.threads = 2;
    sp::UpdateReport r1 = lib->update(opt, err);
    REQUIRE(err.empty());
    CHECK(r1.added == 4);
    CHECK(r1.analyzed == 4);
    CHECK(r1.errors == 0);
    CHECK(lib->count_files() == 4);

    // second update: nothing analyzed
    sp::UpdateReport r2 = lib->update(opt, err);
    CHECK(r2.analyzed == 0);
    CHECK(r2.unchanged == 4);

    // export == scan, byte for byte (§5.2)
    sp::Manifest scanned = sp::scan_directory(t.root, sp::ScanOptions{});
    sp::Manifest exported = lib->export_manifest(nullptr, t.root.generic_string(), err);
    CHECK(sp::manifest_to_json(scanned) == sp::manifest_to_json(exported));

    // move keeps the row (same sha, new path), removal marks missing, prune deletes
    fs::rename(t.root / "tone.wav", t.root / "combat" / "tone_moved.wav");
    fs::remove(t.root / "ui" / "menu_click_confirm.wav");
    sp::UpdateReport r3 = lib->update(opt, err);
    CHECK(r3.moved == 1);
    CHECK(r3.missing == 1);
    CHECK(r3.analyzed == 0);
    CHECK(lib->count_files(true) == 3);
    CHECK(lib->count_files(false) == 4);
    auto moved = lib->get("combat/tone_moved.wav");
    REQUIRE(moved.has_value());
    CHECK(moved->entry.path == "combat/tone_moved.wav");
    CHECK(lib->prune() == 1);
    CHECK(lib->count_files(false) == 3);

    // a changed file is re-analyzed
    {
        std::ofstream f(t.root / "combat" / "tone_moved.wav", std::ios::binary | std::ios::app);
        f << "junk";
    }
    fs::last_write_time(t.root / "combat" / "tone_moved.wav",
                        fs::file_time_type::clock::now() + std::chrono::seconds(5));
    sp::UpdateReport r4 = lib->update(opt, err);
    CHECK(r4.changed == 1);
    CHECK(r4.analyzed == 1);
}

TEST_CASE("library/annotations_precedence_and_search") {
    TempRoot t;
    std::string err;
    auto lib = sp::Library::create(t.root, err);
    REQUIRE(lib != nullptr);
    lib->update(sp::UpdateOptions{}, err);
    REQUIRE(err.empty());

    // §8 offline pass: the UCS-named file and the token-matched one; the rest unmatched.
    sp::ClassifyReport c = lib->classify_offline(false);
    CHECK(c.examined == 4);
    CHECK(c.written ==
          3); // UCS-named, "menu_click_confirm" -> UIClick, "gun_pistol_shot" -> GUNPis
    CHECK(c.unmatched == 1); // tone.wav
    auto ucs_named = lib->get("ui/UIClick_Soft Tap_SP_TEST.wav");
    REQUIRE(ucs_named.has_value());
    REQUIRE(ucs_named->annotation.has_value());
    CHECK(ucs_named->annotation->cat_id == "UIClick");
    CHECK(ucs_named->annotation->fx_name == "Soft Tap");
    CHECK(ucs_named->annotation->confidence == doctest::Approx(1.0));
    CHECK(ucs_named->annotation->source == sp::AnnotationSource::kFilename);
    auto tok = lib->get("combat/gun_pistol_shot_01.wav");
    REQUIRE(tok->annotation.has_value());
    CHECK(tok->annotation->cat_id == "GUNPis");
    CHECK(tok->annotation->fx_name == "Gun Pistol Shot");
    CHECK(tok->annotation->confidence == doctest::Approx(0.4));
    CHECK_FALSE(lib->get("tone.wav")->annotation.has_value());

    // §6.2 precedence: folder < filename < metadata < model < human
    sp::Annotation folder;
    folder.cat_id = "TOONImpt";
    folder.source = sp::AnnotationSource::kFolder;
    CHECK(lib->set_annotation("combat/gun_pistol_shot_01.wav", folder, false) ==
          sp::SetResult::kSkippedLowerPrecedence);
    CHECK(lib->set_annotation("combat/gun_pistol_shot_01.wav", folder, true) ==
          sp::SetResult::kWritten);
    sp::Annotation model;
    model.cat_id = "GUNPis";
    model.description = "A single dry pistol shot with a short room tail.";
    model.keywords = {"pistol", "shot", "dry"};
    model.confidence = 0.8;
    model.source = sp::AnnotationSource::kModel;
    model.model = "mock";
    CHECK(lib->set_annotation("combat/gun_pistol_shot_01.wav", model, false) ==
          sp::SetResult::kWritten);
    CHECK(lib->set_annotation("nope.wav", model, false) == sp::SetResult::kUnknownPath);

    // human write locks; nothing else gets through, not even forced
    sp::Annotation human = model;
    human.source = sp::AnnotationSource::kHuman;
    human.cat_id = "GUNHndl";
    CHECK(lib->set_annotation("combat/gun_pistol_shot_01.wav", human, false) ==
          sp::SetResult::kWritten);
    auto row = lib->get("combat/gun_pistol_shot_01.wav");
    CHECK(row->annotation->locked);
    CHECK(row->annotation->cat_id == "GUNHndl");
    CHECK_FALSE(row->annotation->annotated_at.empty());
    CHECK(lib->set_annotation("combat/gun_pistol_shot_01.wav", model, true) ==
          sp::SetResult::kSkippedLocked);
    CHECK(lib->classify_offline(true).skipped_locked == 1);
    CHECK(lib->set_locked("combat/gun_pistol_shot_01.wav", false));
    CHECK(lib->set_annotation("combat/gun_pistol_shot_01.wav", model, false) ==
          sp::SetResult::kSkippedLowerPrecedence); // human still outranks model when unlocked
    CHECK(lib->set_annotation("combat/gun_pistol_shot_01.wav", model, true) ==
          sp::SetResult::kWritten);

    // §5.4 search: FTS over description/keywords/path, filters, determinism
    sp::SearchQuery q;
    q.text = "pistol";
    auto hits = lib->search(q, err);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].path == "combat/gun_pistol_shot_01.wav");
    q.text = "room tail";
    CHECK(lib->search(q, err).size() == 1);
    q.text = "roo"; // prefix on the last token
    CHECK(lib->search(q, err).size() == 1);
    q.text = "click";
    CHECK(lib->search(q, err).size() == 2); // path tokens of both ui files
    q.text = "\"); DROP TABLE files; --";
    CHECK(lib->search(q, err).size() == 0);
    CHECK(err.empty());
    q = sp::SearchQuery{};
    q.category = "GUNS";
    CHECK(lib->search(q, err).size() == 1);
    q = sp::SearchQuery{};
    q.cat_id_glob = "UI*";
    CHECK(lib->search(q, err).size() == 2); // the UCS-named file + menu_click_confirm
    q = sp::SearchQuery{};
    q.unannotated = true;
    CHECK(lib->search(q, err).size() == 1);
    q = sp::SearchQuery{};
    q.min_confidence = 0.5;
    CHECK(lib->search(q, err).size() == 2); // 1.0 (ucs-named) + 0.8 (model)
    auto a = lib->search(sp::SearchQuery{}, err);
    auto b = lib->search(sp::SearchQuery{}, err);
    REQUIRE(a.size() == b.size());
    for (std::size_t k = 0; k < a.size(); ++k) {
        CHECK(a[k].path == b[k].path);
    }
    CHECK(lib->category_counts().size() == 2);
    CHECK(lib->count_annotated() == 3);

    // filtered export carries only the matching rows and recomputes stats
    sp::SearchQuery only_guns;
    only_guns.category = "GUNS";
    sp::Manifest m = lib->export_manifest(&only_guns, "x", err);
    CHECK(m.files.size() == 1);
    CHECK(m.root == "x");
}

TEST_CASE("library/manifest_json_roundtrip") {
    sp::Manifest m = sp::scan_directory("tests/golden/fixtures", sp::ScanOptions{});
    std::string text = sp::manifest_to_json(m);
    std::string err;
    auto back = sp::manifest_from_json(text, err);
    REQUIRE_MESSAGE(back.has_value(), err);
    // The files array round-trips exactly; stats are recomputed from 4-decimal values and
    // drift (atk01 is steep in attack_s, which is stored with 4 decimals), which is exactly why
    // the library stores entries unrounded (§5.2).
    nlohmann::json a = nlohmann::json::parse(text);
    nlohmann::json b = nlohmann::json::parse(sp::manifest_to_json(*back));
    CHECK(a["files"] == b["files"]);
    CHECK(a["root"] == b["root"]);
    CHECK(a["engine_version"] == b["engine_version"]);
    for (auto &[dim, st] : a["stats"].items()) {
        for (const char *k : {"mean", "std", "min", "max"}) {
            CHECK(std::abs(st[k].get<double>() - b["stats"][dim][k].get<double>()) < 2e-3);
        }
    }
    // full-precision entry -> rounded is identical to a direct rounded serialization
    for (const sp::FileEntry &e : m.files) {
        auto e2 = sp::file_entry_from_json_string(sp::file_entry_to_json_string(e, false), err);
        REQUIRE(e2.has_value());
        CHECK(sp::file_entry_to_json_string(*e2, true) == sp::file_entry_to_json_string(e, true));
    }
}
