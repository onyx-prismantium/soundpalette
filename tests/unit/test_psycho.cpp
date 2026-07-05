// Extension-3 §5 gate table: definitional rows (the units' own reference signals) and oracle
// rows (tests/golden/psycho_reference.json). Tolerances are spec — never widened.
//
// Requires generated fixtures: tests/golden/fixtures (base set) and fixtures_psycho
// (genfixtures --psycho fixtures_psycho).

#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "soundpalette/audio.h"
#include "soundpalette/psycho.h"

#include "../../core/src/psycho/psycho_internal.h"

namespace {

sp::PsychoFeatures psycho_of(const std::string &path) {
    std::string err;
    auto buf = sp::decode_file(path, err);
    REQUIRE_MESSAGE(buf.has_value(), path << ": " << err);
    return sp::compute_psycho(*buf, sp::MappingConfig{});
}

// Stationary loudness via the time-averaged band levels (the §5 stationary gate rows compare
// against the oracle's stationary method).
double stationary_sones(const std::string &path) {
    std::string err;
    auto buf = sp::decode_file(path, err);
    REQUIRE_MESSAGE(buf.has_value(), path << ": " << err);
    sp::MappingConfig cfg;
    const double k = sp::psycho::kP0 * std::pow(10.0, (cfg.ref_spl + 23.0) / 20.0);
    std::vector<float> pa(buf->samples48k_mono.size());
    for (std::size_t i = 0; i < pa.size(); ++i) {
        pa[i] = static_cast<float>(static_cast<double>(buf->samples48k_mono[i]) * k);
    }
    sp::psycho::TvAnalysis tv = sp::psycho::analyze_time_varying(pa, 48000);
    return sp::psycho::loudness_from_third_octave(tv.stationary_levels, nullptr);
}

nlohmann::json load_reference() {
    std::ifstream f("tests/golden/psycho_reference.json");
    REQUIRE_MESSAGE(f.good(), "missing tests/golden/psycho_reference.json");
    std::stringstream ss;
    ss << f.rdbuf();
    return nlohmann::json::parse(ss.str());
}

} // namespace

TEST_CASE("psycho/definitional_loudness") {
    const double n40 = stationary_sones("fixtures_psycho/tone1k_40db.wav");
    CHECK_MESSAGE(std::fabs(n40 - 1.0) <= 0.08, "1 kHz @ 40 dB SPL = " << n40 << " sone");
    const double n50 = stationary_sones("fixtures_psycho/tone1k_50db.wav");
    const double ratio = n50 / n40;
    CHECK_MESSAGE(ratio >= 1.7, "10 dB doubling ratio " << ratio);
    CHECK_MESSAGE(ratio <= 2.3, "10 dB doubling ratio " << ratio);
}

TEST_CASE("psycho/definitional_sharpness") {
    const sp::PsychoFeatures p = psycho_of("fixtures_psycho/nbnoise1k_60db.wav");
    CHECK_MESSAGE(std::fabs(p.sharpness_acum - 1.0) <= 0.10,
                  "narrowband noise = " << p.sharpness_acum << " acum");
}

TEST_CASE("psycho/definitional_roughness_fluctuation") {
    const sp::PsychoFeatures am70 = psycho_of("fixtures_psycho/am70_60db.wav");
    CHECK_MESSAGE(std::fabs(am70.roughness_asper - 1.0) <= 0.20,
                  "70 Hz AM = " << am70.roughness_asper << " asper");
    const sp::PsychoFeatures am4 = psycho_of("fixtures_psycho/am4_60db.wav");
    CHECK_MESSAGE(am4.fluctuation_vacil >= 0.7, "4 Hz AM = " << am4.fluctuation_vacil);
    CHECK_MESSAGE(am4.fluctuation_vacil <= 1.3, "4 Hz AM = " << am4.fluctuation_vacil);
    CHECK_MESSAGE(am4.roughness_asper < 0.3, "4 Hz AM roughness " << am4.roughness_asper);
    CHECK(am4.experimental_fluctuation);
    const sp::PsychoFeatures tone = psycho_of("fixtures_psycho/tone1k_60db.wav");
    CHECK_MESSAGE(tone.roughness_asper < 0.1, "pure tone " << tone.roughness_asper << " asper");
}

TEST_CASE("psycho/oracle_stationary") {
    nlohmann::json ref = load_reference();
    const char *stationary[] = {"tone1k_40db.wav",    "tone1k_50db.wav", "tone1k_60db.wav",
                                "nbnoise1k_60db.wav", "sine997_cal.wav", "noise_white_1s.wav"};
    for (const char *name : stationary) {
        const std::string path = std::string(name).find("sine997") != std::string::npos ||
                                         std::string(name).find("noise_white") != std::string::npos
                                     ? std::string("tests/golden/fixtures/") + name
                                     : std::string("fixtures_psycho/") + name;
        const double zwst = ref["files"][name]["loudness_zwst_sone"].get<double>();
        const double mine = stationary_sones(path);
        CHECK_MESSAGE(std::fabs(mine / zwst - 1.0) <= 0.05,
                      name << ": stationary " << mine << " vs oracle " << zwst);
        const double osharp = ref["files"][name]["sharpness_din_acum"].get<double>();
        const double msharp = psycho_of(path).sharpness_acum;
        CHECK_MESSAGE(std::fabs(msharp / osharp - 1.0) <= 0.05,
                      name << ": sharpness " << msharp << " vs oracle " << osharp);
    }
}

TEST_CASE("psycho/oracle_transient_n5") {
    nlohmann::json ref = load_reference();
    const char *transient[] = {"click.wav", "decay_t60.wav", "darkset/dark_00.wav"};
    for (const char *name : transient) {
        const std::string path = std::string("tests/golden/fixtures/") + name;
        const double on5 = ref["files"][name]["loudness_zwtv_n5_sone"].get<double>();
        const double mine = psycho_of(path).sones_n5;
        CHECK_MESSAGE(std::fabs(mine / on5 - 1.0) <= 0.10,
                      name << ": N5 " << mine << " vs oracle " << on5);
    }
}

TEST_CASE("psycho/oracle_am_roughness") {
    nlohmann::json ref = load_reference();
    for (const char *name : {"am70_60db.wav", "am4_60db.wav"}) {
        const double org = ref["files"][name]["roughness_dw_asper"].get<double>();
        const double mine = psycho_of(std::string("fixtures_psycho/") + name).roughness_asper;
        const double tol = std::max(0.15 * org, 0.05); // ±15 % relative or ±0.05 asper
        CHECK_MESSAGE(std::fabs(mine - org) <= tol,
                      name << ": roughness " << mine << " vs oracle " << org);
    }
}

TEST_CASE("psycho/direction_sharpness") {
    const double bright = psycho_of("tests/golden/fixtures/bright_outlier.wav").sharpness_acum;
    const double dark = psycho_of("tests/golden/fixtures/darkset/dark_00.wav").sharpness_acum;
    CHECK_MESSAGE(bright > dark, "bright " << bright << " !> dark " << dark);
}

TEST_CASE("psycho/determinism") {
    const sp::PsychoFeatures a = psycho_of("tests/golden/fixtures/click.wav");
    const sp::PsychoFeatures b = psycho_of("tests/golden/fixtures/click.wav");
    CHECK(a.sones_n5 == b.sones_n5);
    CHECK(a.sones_mean == b.sones_mean);
    CHECK(a.sharpness_acum == b.sharpness_acum);
    CHECK(a.roughness_asper == b.roughness_asper);
    CHECK(a.fluctuation_vacil == b.fluctuation_vacil);
}

TEST_CASE("psycho/silent_and_scan_flag") {
    // Empty/silent input yields a zeroed block with the convention stamped.
    sp::AudioBuffer empty;
    sp::PsychoFeatures p = sp::compute_psycho(empty, sp::MappingConfig{});
    CHECK(p.ref_spl == doctest::Approx(75.0));
    CHECK(p.sones_n5 == 0.0);
    CHECK(p.experimental_fluctuation);
}
