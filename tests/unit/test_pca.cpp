#include <doctest/doctest.h>

#include <cmath>

#include "soundpalette/pca.h"

TEST_CASE("pca/deterministic") {
    std::vector<std::array<double, 8>> points;
    for (int i = 0; i < 20; ++i) {
        std::array<double, 8> p{};
        const double t = 0.05 * i;
        p[0] = 0.3 + t;
        p[1] = 0.5 - 0.5 * t;
        p[2] = 0.2 + 0.1 * std::sin(i); // deterministic wiggle
        p[3] = 0.4;
        p[4] = 0.6 + 0.02 * i;
        p[5] = 0.5;
        p[6] = 0.1;
        points.push_back(p);
    }

    sp::Pca2 a = sp::compute_pca2(points);
    sp::Pca2 b = sp::compute_pca2(points);
    REQUIRE(a.valid);
    for (std::size_t d = 0; d < 8; ++d) {
        CHECK(a.pc1[d] == b.pc1[d]); // bit-identical across runs
        CHECK(a.pc2[d] == b.pc2[d]);
    }

    // Sign convention: the largest-|value| entry of each component is positive.
    auto largest_positive = [](const std::array<double, 8> &v) {
        std::size_t largest = 0;
        for (std::size_t d = 1; d < 8; ++d) {
            if (std::fabs(v[d]) > std::fabs(v[largest])) {
                largest = d;
            }
        }
        return v[largest] > 0.0;
    };
    CHECK(largest_positive(a.pc1));
    CHECK(largest_positive(a.pc2));

    // Orthogonal components.
    double dot = 0.0;
    for (std::size_t d = 0; d < 8; ++d) {
        dot += a.pc1[d] * a.pc2[d];
    }
    CHECK(std::fabs(dot) < 1e-9);
}

TEST_CASE("pca/direction") {
    // dim0 = t, dim1 = t, others constant -> PC1 must align with (e0+e1)/sqrt(2) (§8).
    std::vector<std::array<double, 8>> points;
    for (double t = 0.1; t <= 0.9 + 1e-9; t += 0.1) {
        std::array<double, 8> p{};
        p[0] = t;
        p[1] = t;
        p[2] = 0.5;
        p[3] = 0.5;
        p[4] = 0.5;
        p[5] = 0.5;
        p[6] = 0.5;
        points.push_back(p);
    }
    // A perfectly collinear set is rank-1, which is exactly the §7.2 fallback case; give
    // dim2 a small alternating variance so lambda2/lambda1 clears 1e-6 without moving PC1
    // beyond the assertion tolerance.
    for (std::size_t i = 0; i < points.size(); ++i) {
        points[i][2] += (i % 2 == 0 ? 1.0 : -1.0) * 0.002;
    }

    sp::Pca2 pca = sp::compute_pca2(points);
    const double inv_sqrt2 = 1.0 / std::sqrt(2.0);
    const double alignment = pca.pc1[0] * inv_sqrt2 + pca.pc1[1] * inv_sqrt2;
    CHECK(std::fabs(alignment) > 0.99);
}

TEST_CASE("pca/fallback on degenerate sets") {
    std::vector<std::array<double, 8>> two_points(2, {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7});
    CHECK_FALSE(sp::compute_pca2(two_points).valid); // < 3 points

    std::vector<std::array<double, 8>> identical(10, {0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5});
    CHECK_FALSE(sp::compute_pca2(identical).valid); // zero spread
}

TEST_CASE("ellipse/eigen2x2") {
    // Hand case (§8): cov [[0.04, 0.018], [0.018, 0.02]].
    sp::Eigen2 e = sp::eigen2x2(0.04, 0.018, 0.02);
    CHECK(e.lambda1 == doctest::Approx(0.05059).epsilon(0.04)); // +-0.002 absolute
    CHECK(e.lambda2 == doctest::Approx(0.00941).epsilon(0.22));
    CHECK(e.theta * 180.0 / 3.14159265358979323846 == doctest::Approx(30.5).epsilon(0.066));
}
