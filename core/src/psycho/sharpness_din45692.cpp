// DIN 45692 sharpness: the g(z)-weighted first moment of the specific-loudness pattern.
// S = 0.11 * Σ N'(z)·g(z)·z·Δz / N, with g(z) = 1 up to 15.8 bark and the standard's
// exponential emphasis above.

#include "psycho_internal.h"

#include <cmath>

namespace sp::psycho {

double sharpness_din_from_ns(const std::array<double, kNumBarkSteps> &ns, double n_total) {
    if (n_total <= 0.0) {
        return 0.0;
    }
    double num = 0.0;
    for (int iz = 0; iz < kNumBarkSteps; ++iz) {
        const double z = 0.1 * (iz + 1);
        const double g = z <= 15.8 ? 1.0 : 0.15 * std::exp(0.42 * (z - 15.8)) + 0.85;
        num += ns[static_cast<std::size_t>(iz)] * g * z * 0.1;
    }
    return 0.11 * num / n_total;
}

} // namespace sp::psycho
