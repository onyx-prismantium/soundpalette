#include <doctest/doctest.h>

#include <array>
#include <cmath>

#include "soundpalette/lint.h"
#include "soundpalette/manifest.h"
#include "soundpalette/mapping.h"

namespace {

// A plausible non-silent file entry whose mapping dims land strictly inside every lin01/log01
// range (no clamping), so shifting a baseline mean by a known amount moves |z| predictably.
sp::FileEntry make_entry(const std::string &path) {
    sp::FileEntry fe;
    fe.path = path;
    fe.loudness.lufs_i = -20.0;
    fe.loudness.true_peak_db = -3.0;
    fe.loudness.silent = false;
    fe.features.centroid_hz = 1200.0;
    fe.features.rolloff85_hz = 3000.0;
    fe.features.flatness = 0.25;
    fe.features.zcr = 0.05;
    fe.features.attack_s = 0.02;
    fe.features.tail_s = 0.5;
    fe.features.roughness = 0.1;
    fe.features.warmth = 0.4;
    fe.visual = sp::map_v1(fe.features, fe.loudness, 1);
    return fe;
}

// Baseline whose per-dim mean equals the entry's own dims (z == 0 everywhere) with a given std.
sp::Manifest baseline_centered_on(const sp::FileEntry &fe, double std) {
    sp::Manifest baseline;
    std::array<double, 7> dims = sp::mapping_dims(fe.features, fe.loudness);
    for (std::size_t d = 0; d < 7; ++d) {
        baseline.stats[d].mean = dims[d];
        baseline.stats[d].std = std;
        baseline.stats[d].min = dims[d];
        baseline.stats[d].max = dims[d];
    }
    return baseline;
}

} // namespace

TEST_CASE("lint/pass when candidate matches baseline") {
    sp::FileEntry fe = make_entry("a.wav");
    sp::Manifest baseline = baseline_centered_on(fe, 0.1);
    sp::Manifest candidate;
    candidate.files.push_back(fe);

    sp::LintReport report = sp::lint(baseline, candidate, 2.5);
    CHECK(report.outliers.empty());
    CHECK(report.considered_files == 1);
}

TEST_CASE("lint/outlier when a dimension drifts past threshold") {
    sp::FileEntry fe = make_entry("bright.wav");
    sp::Manifest baseline = baseline_centered_on(fe, 0.1);
    // Shift the bright01 mean 3 sigma below the candidate's value: z = +3.0 >= 2.5.
    baseline.stats[static_cast<std::size_t>(sp::LintDim::kBright01)].mean -= 0.3;

    sp::Manifest candidate;
    candidate.files.push_back(fe);

    sp::LintReport report = sp::lint(baseline, candidate, 2.5);
    REQUIRE(report.outliers.size() == 1);
    CHECK(report.outliers[0].path == "bright.wav");
    CHECK(report.outliers[0].worst_dim == "bright01");
    CHECK(report.outliers[0].worst_z == doctest::Approx(3.0).epsilon(0.01));
    REQUIRE(report.outliers[0].offending_dims.size() == 1);
    CHECK(report.outliers[0].offending_dims[0] == "bright01");
}

TEST_CASE("lint/std floored at 0.02") {
    sp::FileEntry fe = make_entry("a.wav");
    // Degenerate baseline (std == 0 everywhere). Without the floor every nonzero deviation
    // would be an infinite z; with the floor a 0.01 shift is z = 0.5 -> not an outlier.
    sp::Manifest baseline = baseline_centered_on(fe, 0.0);
    baseline.stats[static_cast<std::size_t>(sp::LintDim::kWarm01)].mean -= 0.01;

    sp::Manifest candidate;
    candidate.files.push_back(fe);

    sp::LintReport report = sp::lint(baseline, candidate, 2.5);
    CHECK(report.outliers.empty());

    // But a 0.1 shift against the floored std is z = 5.0 -> outlier.
    baseline.stats[static_cast<std::size_t>(sp::LintDim::kWarm01)].mean -= 0.09;
    report = sp::lint(baseline, candidate, 2.5);
    REQUIRE(report.outliers.size() == 1);
    CHECK(report.outliers[0].worst_dim == "warm01");
    CHECK(report.outliers[0].worst_z == doctest::Approx(5.0).epsilon(0.01));
}

TEST_CASE("lint/outliers sorted by |worst_z| descending") {
    sp::FileEntry small = make_entry("small.wav");
    sp::FileEntry big = make_entry("big.wav");
    big.features.tail_s = 2.5; // pushes tail01 far above the baseline mean
    big.visual = sp::map_v1(big.features, big.loudness, 2);

    sp::Manifest baseline = baseline_centered_on(small, 0.05);
    baseline.stats[static_cast<std::size_t>(sp::LintDim::kWarm01)].mean -= 0.15; // small: z=3

    sp::Manifest candidate;
    candidate.files.push_back(small);
    candidate.files.push_back(big);

    sp::LintReport report = sp::lint(baseline, candidate, 2.5);
    REQUIRE(report.outliers.size() == 2);
    CHECK(report.outliers[0].path == "big.wav");
    CHECK(std::fabs(report.outliers[0].worst_z) >= std::fabs(report.outliers[1].worst_z));
}

TEST_CASE("lint/error and silent files are excluded") {
    sp::FileEntry good = make_entry("good.wav");

    sp::FileEntry broken = make_entry("broken.wav");
    broken.error = "decode failed";

    sp::FileEntry quiet = make_entry("quiet.wav");
    quiet.loudness.silent = true;

    sp::Manifest baseline = baseline_centered_on(good, 0.1);
    // Shift every mean so any considered file would be a wild outlier.
    for (std::size_t d = 0; d < 7; ++d) {
        baseline.stats[d].mean += 10.0;
    }

    sp::Manifest candidate;
    candidate.files.push_back(broken);
    candidate.files.push_back(quiet);

    sp::LintReport report = sp::lint(baseline, candidate, 2.5);
    CHECK(report.considered_files == 0);
    CHECK(report.outliers.empty());
}
