#include "soundpalette/lint.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "soundpalette/mapping.h"

namespace sp {

namespace {
constexpr double kStdFloor = 0.02;
constexpr const char *kDimNames[7] = {"bright01", "warm01", "ton01",   "atk01",
                                      "tail01",   "loud01", "jitter01"};
} // namespace

LintReport lint(const Manifest &baseline, const Manifest &candidate, double threshold) {
    LintReport report;

    for (const FileEntry &fe : candidate.files) {
        if (!fe.error.empty() || fe.loudness.silent) {
            continue;
        }
        ++report.considered_files;

        std::array<double, 7> dims = mapping_dims(fe.features, fe.loudness);

        double worst_z = 0.0;
        double worst_abs_z = -1.0;
        int worst_idx = 0;
        std::vector<std::string> offending;

        for (int d = 0; d < 7; ++d) {
            double std_floored =
                std::max(baseline.stats[static_cast<std::size_t>(d)].std, kStdFloor);
            double z = (dims[static_cast<std::size_t>(d)] -
                        baseline.stats[static_cast<std::size_t>(d)].mean) /
                       std_floored;
            if (std::fabs(z) > worst_abs_z) {
                worst_abs_z = std::fabs(z);
                worst_z = z;
                worst_idx = d;
            }
            if (std::fabs(z) >= threshold) {
                offending.emplace_back(kDimNames[d]);
            }
        }

        if (!offending.empty()) {
            LintFileResult r;
            r.path = fe.path;
            r.worst_dim = kDimNames[worst_idx];
            r.worst_z = worst_z;
            r.offending_dims = std::move(offending);
            report.outliers.push_back(std::move(r));
        }
    }

    std::sort(report.outliers.begin(), report.outliers.end(),
              [](const LintFileResult &a, const LintFileResult &b) {
                  return std::fabs(a.worst_z) > std::fabs(b.worst_z);
              });

    return report;
}

} // namespace sp
