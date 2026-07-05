#include "soundpalette/describe.h"

#include "soundpalette/mapping.h"

namespace sp {

namespace {

// Extension §5.1: five bins per dim with edges 0.2 / 0.4 / 0.6 / 0.8, fixed word tables.
constexpr const char *kWordTables[8][5] = {
    {"very dark", "dark", "neutral", "bright", "very bright"}, // bright01
    {"cold", "cool", "neutral", "warm", "very warm"},          // warm01
    {"pure noise", "noisy", "mixed", "tonal", "very tonal"},   // ton01
    {"very soft attack", "soft attack", "moderate attack", "hard attack",
     "instant attack"},                                                         // atk01
    {"dry", "short tail", "medium tail", "long tail", "very long tail"},        // tail01
    {"very quiet", "quiet", "moderate level", "loud", "very loud"},             // loud01
    {"smooth", "slightly gritty", "gritty", "very gritty", "harsh"},            // jitter01
    {"steady", "gently breathing", "pulsing", "wobbling", "strongly wobbling"}, // fluct01
};

int bin_index(double v) {
    if (v < 0.2) {
        return 0;
    }
    if (v < 0.4) {
        return 1;
    }
    if (v < 0.6) {
        return 2;
    }
    if (v < 0.8) {
        return 3;
    }
    return 4;
}

} // namespace

std::array<std::string, 8> describe_dim_words(const Features &features, const Loudness &loudness,
                                              const PsychoFeatures &psycho) {
    std::array<double, 8> dims = mapping_dims(features, loudness, psycho);
    std::array<std::string, 8> words;
    for (std::size_t d = 0; d < 8; ++d) {
        words[d] = kWordTables[d][bin_index(dims[d])];
    }
    return words;
}

std::string describe_words(const Features &features, const Loudness &loudness,
                           const PsychoFeatures &psycho) {
    if (loudness.silent) {
        return "silent.";
    }
    std::array<std::string, 8> w = describe_dim_words(features, loudness, psycho);
    // Sentence template (extension §5.1 order, + the v2 fluctuation word before the period).
    return w[0] + ", " + w[1] + ", " + w[2] + "; " + w[3] + ", " + w[4] + "; " + w[5] + ", " +
           w[6] + ", " + w[7] + ".";
}

} // namespace sp
