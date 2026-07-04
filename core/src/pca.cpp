// Deterministic PCA + 2x2 eigen solve (extension-2 §7.2). Core owns the math so the GUI's
// constellation view stays thin and unit-testable.

#include <cmath>

#include "soundpalette/pca.h"

namespace sp {

namespace {

using Vec7 = std::array<double, 7>;
using Mat7 = std::array<std::array<double, 7>, 7>;

Vec7 mat_vec(const Mat7 &m, const Vec7 &v) {
    Vec7 out{};
    for (std::size_t a = 0; a < 7; ++a) {
        for (std::size_t b = 0; b < 7; ++b) {
            out[a] += m[a][b] * v[b];
        }
    }
    return out;
}

double dot(const Vec7 &a, const Vec7 &b) {
    double s = 0.0;
    for (std::size_t d = 0; d < 7; ++d) {
        s += a[d] * b[d];
    }
    return s;
}

bool normalize(Vec7 &v) {
    const double n = std::sqrt(dot(v, v));
    if (n < 1e-15) {
        return false;
    }
    for (double &x : v) {
        x /= n;
    }
    return true;
}

// Sign convention (§7.2): flip so the component's largest-|value| entry is positive.
void fix_sign(Vec7 &v) {
    std::size_t largest = 0;
    for (std::size_t d = 1; d < 7; ++d) {
        if (std::fabs(v[d]) > std::fabs(v[largest])) {
            largest = d;
        }
    }
    if (v[largest] < 0.0) {
        for (double &x : v) {
            x = -x;
        }
    }
}

} // namespace

Pca2 compute_pca2(const std::vector<Vec7> &points) {
    Pca2 result;
    if (points.size() < 3) {
        return result; // fallback signaled via valid == false
    }

    for (const Vec7 &p : points) {
        for (std::size_t d = 0; d < 7; ++d) {
            result.mean[d] += p[d];
        }
    }
    for (std::size_t d = 0; d < 7; ++d) {
        result.mean[d] /= static_cast<double>(points.size());
    }

    Mat7 cov{};
    for (const Vec7 &p : points) {
        for (std::size_t a = 0; a < 7; ++a) {
            for (std::size_t b = 0; b < 7; ++b) {
                cov[a][b] += (p[a] - result.mean[a]) * (p[b] - result.mean[b]);
            }
        }
    }
    for (auto &row : cov) {
        for (double &x : row) {
            x /= static_cast<double>(points.size());
        }
    }

    // PC1: power iteration, fixed start and iteration count for determinism.
    Vec7 v1;
    v1.fill(1.0 / std::sqrt(7.0));
    for (int it = 0; it < 200; ++it) {
        v1 = mat_vec(cov, v1);
        if (!normalize(v1)) {
            return result;
        }
    }
    result.lambda1 = dot(v1, mat_vec(cov, v1));

    // PC2: deflation, re-orthogonalized against v1 and normalized each iteration.
    Mat7 deflated = cov;
    for (std::size_t a = 0; a < 7; ++a) {
        for (std::size_t b = 0; b < 7; ++b) {
            deflated[a][b] -= result.lambda1 * v1[a] * v1[b];
        }
    }
    Vec7 v2;
    for (std::size_t d = 0; d < 7; ++d) {
        v2[d] = 1.0 / std::sqrt(7.0) * (d % 2 == 0 ? 1.0 : -1.0); // deterministic start
    }
    for (int it = 0; it < 200; ++it) {
        v2 = mat_vec(deflated, v2);
        const double along = dot(v2, v1);
        for (std::size_t d = 0; d < 7; ++d) {
            v2[d] -= along * v1[d];
        }
        if (!normalize(v2)) {
            return result;
        }
    }
    result.lambda2 = dot(v2, mat_vec(cov, v2));

    // Components are stored even when degenerate — the valid flag is the §7.2 fallback
    // signal for the VIEW (don't offer PCA axes), not a claim that PC1 is meaningless.
    fix_sign(v1);
    fix_sign(v2);
    result.pc1 = v1;
    result.pc2 = v2;
    result.valid = result.lambda1 > 0.0 && result.lambda2 / result.lambda1 >= 1e-6;
    return result;
}

Eigen2 eigen2x2(double a, double b, double c) {
    Eigen2 e;
    const double half_sum = (a + c) / 2.0;
    const double half_diff = (a - c) / 2.0;
    const double root = std::sqrt(half_diff * half_diff + b * b);
    e.lambda1 = half_sum + root;
    e.lambda2 = half_sum - root;
    e.theta = 0.5 * std::atan2(2.0 * b, a - c);
    return e;
}

} // namespace sp
