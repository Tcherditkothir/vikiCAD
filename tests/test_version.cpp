#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "Version.h"

TEST_CASE("version strings are non-empty", "[version]")
{
    REQUIRE(std::strlen(viki::versionString()) > 0);
    REQUIRE(std::strlen(viki::occtVersionString()) > 0);
}
