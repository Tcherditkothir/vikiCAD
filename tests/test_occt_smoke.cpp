#include <catch2/catch_test_macros.hpp>

#include "solid/OcctOps.h"

TEST_CASE("OCCT links and builds a box", "[occt]")
{
    REQUIRE(viki::occtSmokeTest());
}
