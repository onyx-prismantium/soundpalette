#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "soundpalette/version.h"

TEST_CASE("version string is well-formed") {
    CHECK(std::string(sp::kVersionString) == "0.1.0");
}
