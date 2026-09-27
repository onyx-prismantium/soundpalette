#include <doctest/doctest.h>

#include <cstdint>
#include <ostream> // doctest stringifies std::string_view operands via operator<< (MSVC needs it)

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "soundpalette/annotator.h"
#include "soundpalette/audio.h"

TEST_CASE("annotator/stage2_shortlist") {
    // §7.2: ranked within the model's category, padded to >= 3, capped at max_n.
    sp::Stage1 s1;
    s1.description = "A single dry pistol shot with a short room tail.";
    s1.fx_name = "Pistol Shot Dry";
    s1.category = "GUNS";
    s1.keywords = {"pistol", "shot", "gun", "dry"};
    std::vector<sp::UcsMatch> sl = sp::stage2_shortlist(s1, 12);
    REQUIRE_FALSE(sl.empty());
    CHECK(sl.front().entry->cat_id == "GUNPis");
    CHECK(sl.size() >= 3);
    CHECK(sl.size() <= 12);
    for (const sp::UcsMatch &m : sl) {
        CHECK(m.entry->category == "GUNS");
    }

    // padding: a category with no keyword hits still yields its first three SubCategories
    sp::Stage1 none;
    none.category = "GUNS";
    none.keywords = {"qzxv"};
    std::vector<sp::UcsMatch> padded = sp::stage2_shortlist(none, 12);
    CHECK(padded.size() == 3);
    CHECK(padded[0].score == 0);
    CHECK(padded[0].entry->cat_id == sp::ucs_in_category("GUNS")[0]->cat_id);

    // unknown category: category-less ranking, no padding
    sp::Stage1 unknown = s1;
    unknown.category = "NOT A CATEGORY";
    std::vector<sp::UcsMatch> all = sp::stage2_shortlist(unknown, 5);
    REQUIRE_FALSE(all.empty());
    CHECK(all.size() <= 5);
    CHECK(all.front().entry->cat_id == "GUNPis");
}

TEST_CASE("annotator/resolve_stage2") {
    sp::Stage1 s1;
    s1.category = "GUNS";
    s1.keywords = {"pistol", "shot"};
    std::vector<sp::UcsMatch> sl = sp::stage2_shortlist(s1, 12);
    REQUIRE(sl.front().entry->cat_id == "GUNPis");

    // in the shortlist: taken as is
    sp::Stage2Decision d = sp::resolve_stage2(sl, "GUNPis", 0.8);
    CHECK(d.cat_id == "GUNPis");
    CHECK(d.confidence == doctest::Approx(0.8));
    CHECK(d.from_shortlist);

    // outside the shortlist but same category: accepted
    bool cano_listed = false;
    for (const sp::UcsMatch &m : sl) {
        cano_listed = cano_listed || m.entry->cat_id == "GUNCano";
    }
    sp::Stage2Decision same = sp::resolve_stage2(sl, "GUNCano", 0.7);
    CHECK(same.cat_id == "GUNCano");
    CHECK(same.confidence == doctest::Approx(0.7));
    CHECK(same.from_shortlist == cano_listed);

    // another category: shortlist top with confidence halved
    sp::Stage2Decision other = sp::resolve_stage2(sl, "TOONBoing", 0.9);
    CHECK(other.cat_id == "GUNPis");
    CHECK(other.confidence == doctest::Approx(0.45));
    CHECK_FALSE(other.note.empty());

    // unknown CatID: same fallback
    sp::Stage2Decision bogus = sp::resolve_stage2(sl, "NOPE", 0.9);
    CHECK(bogus.cat_id == "GUNPis");
    CHECK(bogus.confidence == doctest::Approx(0.45));

    // confidence is clamped
    CHECK(sp::resolve_stage2(sl, "GUNPis", 7.0).confidence == doctest::Approx(1.0));
    // empty shortlist: no CatID
    CHECK(sp::resolve_stage2({}, "GUNPis", 0.9).cat_id.empty());
}

TEST_CASE("annotator/wav16k") {
    // A 1 kHz tone at 48 kHz becomes a 16 kHz mono 16-bit WAV of a third the length, with the
    // tone intact (RMS within 5%) — the model hears what we analyzed.
    sp::AudioBuffer buf;
    buf.src_rate = 48000;
    buf.src_channels = 1;
    buf.duration_s = 0.5;
    buf.samples48k_mono.resize(24000);
    for (std::size_t i = 0; i < buf.samples48k_mono.size(); ++i) {
        buf.samples48k_mono[i] =
            static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979 * 1000.0 * i / 48000.0));
    }
    std::filesystem::path p = std::filesystem::temp_directory_path() / "sp_test_16k.wav";
    std::string err;
    REQUIRE_MESSAGE(sp::write_wav_16k_mono(buf, p, err), err);
    std::ifstream f(p, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    REQUIRE(bytes.size() == 44 + 8000 * 2);
    CHECK(bytes.substr(0, 4) == "RIFF");
    CHECK(bytes.substr(8, 4) == "WAVE");
    auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 16) |
               (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3])) << 24);
    };
    CHECK(u32(24) == 16000u); // sample rate
    CHECK(u32(40) == 16000u); // data bytes
    double sq = 0.0;
    for (std::size_t i = 44 + 200; i + 1 < bytes.size() - 200; i += 2) {
        std::int16_t s = static_cast<std::int16_t>(static_cast<unsigned char>(bytes[i]) |
                                                   (static_cast<unsigned char>(bytes[i + 1]) << 8));
        sq += (s / 32767.0) * (s / 32767.0);
    }
    double rms = std::sqrt(sq / ((bytes.size() - 44 - 400) / 2));
    CHECK(rms == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.05));
    std::filesystem::remove(p);
}

TEST_CASE("annotator/argv") {
    sp::AnnotatorOptions o;
    o.command = "node \"/tmp/my dir/annotate.js\" --model 'qwen 2' --x";
    std::vector<std::string> argv = sp::annotator_argv(o);
    REQUIRE(argv.size() == 5);
    CHECK(argv[0] == "node");
    CHECK(argv[1] == "/tmp/my dir/annotate.js");
    CHECK(argv[3] == "qwen 2");
    CHECK(argv[4] == "--x");
    o.command.clear();
    // falls back to the default name when neither option nor env is set
    if (std::getenv("SP_ANNOTATOR") == nullptr) {
        CHECK(sp::annotator_argv(o) == std::vector<std::string>{sp::kDefaultAnnotatorCommand});
    }
}

TEST_CASE("annotator/unreachable_command") {
    // A missing annotator is a fatal report, never an exception or a hang.
    std::filesystem::path root = std::filesystem::temp_directory_path() / "sp_ann_unreach";
    std::filesystem::create_directories(root);
    std::filesystem::copy_file("tests/golden/fixtures/click.wav", root / "a.wav",
                               std::filesystem::copy_options::overwrite_existing);
    std::string err;
    auto lib = sp::Library::create(root, err);
    REQUIRE(lib != nullptr);
    lib->update(sp::UpdateOptions{}, err);
    sp::AnnotatorOptions o;
    o.command = "sp-no-such-annotator-zzz";
    sp::AnnotateReport r = sp::annotate_paths(*lib, {"a.wav"}, o);
    CHECK_FALSE(r.fatal.empty());
    CHECK(r.errors == 1);
    CHECK(r.annotated == 0);
    lib.reset();
    std::filesystem::remove_all(root);
}
