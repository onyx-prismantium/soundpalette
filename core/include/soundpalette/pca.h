#pragma once

#include <array>
#include <vector>

namespace sp {

// Deterministic 2-component PCA over the 8-dim mapping space (extension-2 §7.2): mean-centered
// (no rescaling), PC1 by power iteration (start (1,...,1)/sqrt(7), 200 iterations), PC2 by
// deflation with re-orthogonalization; sign flipped so each component's largest-|value| entry
// is positive. valid == false when the set has < 3 points or lambda2/lambda1 < 1e-6 — callers
// fall back to the default axis pair.
struct Pca2 {
    std::array<double, 8> pc1{};
    std::array<double, 8> pc2{};
    std::array<double, 8> mean{};
    double lambda1 = 0.0;
    double lambda2 = 0.0;
    bool valid = false;
};

Pca2 compute_pca2(const std::vector<std::array<double, 8>> &points);

// Eigen-decomposition of the symmetric 2x2 covariance [[a, b], [b, c]] (extension-2 §7.2):
// lambda = (a+c)/2 +- sqrt(((a-c)/2)^2 + b^2), theta = 0.5*atan2(2b, a-c). Used for the
// 1-sigma/2-sigma profile ellipses.
struct Eigen2 {
    double lambda1 = 0.0; // major
    double lambda2 = 0.0; // minor
    double theta = 0.0;   // radians, orientation of the major axis
};

Eigen2 eigen2x2(double a, double b, double c);

} // namespace sp
