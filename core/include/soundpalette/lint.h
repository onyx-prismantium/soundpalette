#pragma once

#include <string>
#include <vector>

#include "soundpalette/manifest.h"

namespace sp {

struct LintFileResult {
    std::string path;
    std::string worst_dim;                   // name of the dimension with the largest |z|
    double worst_z = 0.0;                    // signed z-score of that dimension
    std::vector<std::string> offending_dims; // dims where |z| >= threshold, fixed dim order
};

struct LintReport {
    std::vector<LintFileResult> outliers; // sorted by |worst_z| descending
    int considered_files = 0;             // candidate files evaluated (non-error, non-silent)
};

// Compares candidate's files against baseline's stats block in the seven-dimensional mapping
// space [bright01, warm01, ton01, atk01, tail01, loud01, jitter01] (§9). z-score per dimension
// with std floored at 0.02. Files with any |z| >= threshold are outliers. Error/silent candidate
// files are excluded (mirrors the same exclusion manifest stats uses, §8).
LintReport lint(const Manifest &baseline, const Manifest &candidate, double threshold);

} // namespace sp
