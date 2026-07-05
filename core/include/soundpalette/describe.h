#pragma once

#include <array>
#include <string>

#include "soundpalette/audio.h"
#include "soundpalette/features.h"
#include "soundpalette/psycho.h"

namespace sp {

// The word chosen for each of the eight mapping dims (extension §5.1 fixed word tables), in
// the mapping_dims order [bright01, warm01, ton01, atk01, tail01, loud01, jitter01, fluct01].
std::array<std::string, 8> describe_dim_words(const Features &features, const Loudness &loudness,
                                              const PsychoFeatures &psycho);

// Deterministic plain-language sentence for a sound (extension §5.1):
// "<bright>, <warm>, <ton>; <atk>, <tail>; <loud>, <grit>." — or "silent." for silent files.
std::string describe_words(const Features &features, const Loudness &loudness,
                           const PsychoFeatures &psycho);

} // namespace sp
