#include <doctest/doctest.h>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "soundpalette/manifest.h"

namespace {

using json = nlohmann::json;

json load_json_file(const std::filesystem::path &path) {
    std::ifstream f(path);
    REQUIRE_MESSAGE(f.good(), path.string());
    std::stringstream ss;
    ss << f.rdbuf();
    return json::parse(ss.str());
}

// Structural check against docs/manifest.schema.json's required keys/types/ranges (§8): a
// hand-rolled walk rather than pulling in a JSON-Schema validation library (not in §4's
// dependency list, and the plan's own wording only asks for "structural checks").
void check_dim_stats(const json &stats) {
    for (const char *dim :
         {"bright01", "warm01", "ton01", "atk01", "tail01", "loud01", "jitter01"}) {
        REQUIRE(stats.contains(dim));
        const json &d = stats.at(dim);
        for (const char *key : {"mean", "std", "min", "max"}) {
            REQUIRE(d.contains(key));
            CHECK(d.at(key).is_number());
        }
        CHECK(d.at("std").get<double>() >= 0.0);
    }
}

void check_file_entry(const json &fe) {
    for (const char *key : {"path", "sha256", "error", "duration_s", "sample_rate", "channels",
                            "truncated", "loudness", "features", "visual"}) {
        REQUIRE_MESSAGE(fe.contains(key), key);
    }
    CHECK(fe.at("path").is_string());
    CHECK(fe.at("sha256").get<std::string>().size() == 64);
    CHECK(fe.at("duration_s").get<double>() >= 0.0);

    const json &loudness = fe.at("loudness");
    for (const char *key : {"lufs_i", "true_peak_db", "silent"}) {
        REQUIRE(loudness.contains(key));
    }

    const json &features = fe.at("features");
    for (const char *key : {"centroid_hz", "rolloff85_hz", "flatness", "zcr", "attack_s", "tail_s",
                            "tail_clipped", "roughness", "warmth", "bands"}) {
        REQUIRE(features.contains(key));
    }
    CHECK(features.at("bands").size() == 6);
    CHECK(features.at("flatness").get<double>() >= 0.0);
    CHECK(features.at("flatness").get<double>() <= 1.0);

    const json &visual = fe.at("visual");
    for (const char *key : {"hue_deg", "sat", "light", "size_px", "spike01", "spikes", "jitter01",
                            "tail01", "seed"}) {
        REQUIRE(visual.contains(key));
    }
    CHECK(visual.at("hue_deg").get<double>() >= 0.0);
    CHECK(visual.at("hue_deg").get<double>() <= 360.0);
    CHECK(visual.at("sat").get<double>() >= 0.0);
    CHECK(visual.at("sat").get<double>() <= 100.0);
}

} // namespace

TEST_CASE("manifest/schema") {
    const std::filesystem::path golden = "tests/golden/palette.json";
    if (!std::filesystem::exists(golden)) {
        MESSAGE("skipping: ", golden.string(), " not present yet");
        return;
    }
    json m = load_json_file(golden);

    for (const char *key :
         {"schema_version", "mapping_version", "root", "file_count", "files", "stats"}) {
        REQUIRE_MESSAGE(m.contains(key), key);
    }
    CHECK(m.at("schema_version").get<int>() == 2); // v2: psycho block per file
    CHECK(m.at("ref_spl").get<double>() == doctest::Approx(75.0));
    CHECK(m.at("mapping_version").get<int>() == 2);
    CHECK(m.at("file_count").get<std::size_t>() == m.at("files").size());

    for (const auto &fe : m.at("files")) {
        check_file_entry(fe);
    }
    check_dim_stats(m.at("stats"));
}
