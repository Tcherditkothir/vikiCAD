#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "doc/Entities.h"
#include "snap/SnapEngine.h"

using namespace viki;
using Catch::Approx;

namespace {
Document makeDoc()
{
    Document doc;
    doc.beginTransaction(QStringLiteral("setup"));
    doc.addEntity(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{100, 0}));   // id 1
    doc.addEntity(std::make_unique<LineEntity>(Vec2d{50, -50}, Vec2d{50, 50})); // id 2
    doc.addEntity(std::make_unique<CircleEntity>(Vec2d{200, 0}, 25));           // id 3
    doc.commitTransaction();
    return doc;
}
} // namespace

TEST_CASE("endpoint beats midpoint and nearest wins", "[snap]")
{
    const Document doc = makeDoc();
    SnapSettings s;
    const auto r = snapQuery(doc, {99, 1}, 5.0, s, std::nullopt);
    REQUIRE(r);
    REQUIRE(r->kind == SnapKind::Endpoint);
    REQUIRE(r->point.x == Approx(100.0));
}

TEST_CASE("midpoint snap", "[snap]")
{
    const Document doc = makeDoc();
    SnapSettings s;
    const auto r = snapQuery(doc, {49, 2}, 5.0, s, std::nullopt);
    REQUIRE(r);
    // (50,0) is both the midpoint of line 1 and the intersection of 1 and 2 —
    // intersection outranks midpoint.
    REQUIRE(r->kind == SnapKind::Intersection);
    REQUIRE(r->point.x == Approx(50.0));
    REQUIRE(r->point.y == Approx(0.0).margin(1e-9));
}

TEST_CASE("center and quadrant snaps on circle", "[snap]")
{
    const Document doc = makeDoc();
    SnapSettings s;
    const auto center = snapQuery(doc, {201, 1}, 5.0, s, std::nullopt);
    REQUIRE(center);
    REQUIRE(center->kind == SnapKind::Center);

    const auto quad = snapQuery(doc, {224, 1}, 5.0, s, std::nullopt);
    REQUIRE(quad);
    REQUIRE(quad->kind == SnapKind::Quadrant);
    REQUIRE(quad->point.x == Approx(225.0));
}

TEST_CASE("perpendicular snap from a base point", "[snap]")
{
    const Document doc = makeDoc();
    SnapSettings s;
    s.endpoint = s.midpoint = s.center = s.quadrant = s.intersection = false;
    // Base at (30,40): perpendicular foot on line 1 (the X axis) is (30,0).
    const auto r = snapQuery(doc, {29, 1}, 5.0, s, Vec2d{30, 40});
    REQUIRE(r);
    REQUIRE(r->kind == SnapKind::Perpendicular);
    REQUIRE(r->point.x == Approx(30.0));
    REQUIRE(r->point.y == Approx(0.0).margin(1e-9));
}

TEST_CASE("disabled master switch returns nothing", "[snap]")
{
    const Document doc = makeDoc();
    SnapSettings s;
    s.enabled = false;
    REQUIRE_FALSE(snapQuery(doc, {0, 0}, 5.0, s, std::nullopt));
}

TEST_CASE("snap ignores invisible layers", "[snap]")
{
    Document doc = makeDoc();
    const LayerId hidden = doc.ensureLayer(QStringLiteral("hidden"), 0xFFFFFF, false);
    doc.beginTransaction(QStringLiteral("h"));
    auto line = std::make_unique<LineEntity>(Vec2d{300, 0}, Vec2d{400, 0});
    line->setLayerId(hidden);
    doc.addEntity(std::move(line));
    doc.commitTransaction();
    // The entity kept its explicit layer (hidden), so no snap there.
    SnapSettings s;
    REQUIRE_FALSE(snapQuery(doc, {300, 0}, 5.0, s, std::nullopt));
}
