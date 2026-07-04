#include <doctest/doctest.h>

#include "soundpalette/profile.h"

TEST_CASE("glob/semantics") {
    // ** spans path segments (extension-2 §4.2).
    CHECK(sp::glob_match("ui/**", "ui/a/b.wav"));
    CHECK(sp::glob_match("ui/**", "ui/click.wav"));
    CHECK_FALSE(sp::glob_match("ui/**", "combat/ui/x.wav"));

    // **/prefix* reaches into any depth.
    CHECK(sp::glob_match("**/ui_*", "x/y/ui_3.wav"));
    CHECK(sp::glob_match("**/ui_*", "a/ui_click.wav"));

    // * stays within one segment.
    CHECK(sp::glob_match("ui/*.wav", "ui/a.wav"));
    CHECK_FALSE(sp::glob_match("ui/*.wav", "ui/a/b.wav"));

    // ? matches exactly one non-slash character.
    CHECK(sp::glob_match("ui/?.wav", "ui/a.wav"));
    CHECK_FALSE(sp::glob_match("ui/?.wav", "ui/ab.wav"));
    CHECK_FALSE(sp::glob_match("ui?wav", "ui/wav"));

    // Full-anchored: no partial matches; case-sensitive.
    CHECK_FALSE(sp::glob_match("ui", "ui/a.wav"));
    CHECK_FALSE(sp::glob_match("UI/**", "ui/a.wav"));

    // Regex metacharacters in paths are literal.
    CHECK(sp::glob_match("a+b/*.wav", "a+b/c.wav"));
    CHECK_FALSE(sp::glob_match("a.b", "axb"));
}
