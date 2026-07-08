#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QJsonDocument>

#include "doc/EntitiesEx.h"
#include "doc/EntityFactory.h"

using namespace viki;
using Catch::Approx;

namespace {
QByteArray json(const Entity& e)
{
    return QJsonDocument(e.toJson()).toJson(QJsonDocument::Compact);
}
} // namespace

TEST_CASE("polyline with bulge: flatten, bounds, mirror flips bulge", "[entities]")
{
    // (0,0)->(10,0) with bulge 1 (half circle). Positive bulge = CCW arc,
    // which for a +x chord lies BELOW the chord (DXF spec; sagitta = b*c/2).
    PolylineEntity pl({{{0, 0}, 1.0}, {{10, 0}, 0.0}}, false);

    RenderContext ctx;
    ctx.chordTolerance = 0.001;
    PrimitiveList list;
    pl.buildPrimitives(ctx, list);
    REQUIRE(list.strokes.size() == 1);
    const BBox2d b = pl.bounds();
    REQUIRE(b.min.y == Approx(-5.0).margin(1e-6));
    REQUIRE(b.max.y == Approx(0.0).margin(1e-6));

    PolylineEntity mirrored = pl;
    mirrored.transform(Xform2d{1, 0, 0, -1, 0, 0}); // mirror about X axis
    REQUIRE(mirrored.vertices()[0].bulge == Approx(-1.0));
    REQUIRE(mirrored.bounds().max.y == Approx(5.0).margin(1e-6));
}

TEST_CASE("ellipse points and serialization round-trip", "[entities]")
{
    EllipseEntity el({10, 5}, {20, 0}, 0.5);
    REQUIRE(el.pointAt(0).x == Approx(30.0));
    REQUIRE(el.pointAt(M_PI_2).y == Approx(15.0)); // minor = 10

    const QJsonObject obj = el.toJson();
    const auto restored = entityFromJson(obj);
    REQUIRE(restored);
    REQUIRE(json(*restored) == json(el));
}

TEST_CASE("ellipse transform recovers axes (rotation/mirror/non-uniform)",
          "[entities][ellipse]")
{
    // Implicit test of an ellipse: a point P is on ellipse (C, major, ratio)
    // iff ((P-C).majHat/|major|)^2 + ((P-C).minHat/minLen)^2 == 1. Under any
    // affine map, xf(E.pointAt(t)) must land on E.transform(xf) for every t.
    const auto onEllipse = [](const EllipseEntity& e, const Vec2d& p) {
        const Vec2d rel = p - e.center();
        const Vec2d maj = e.majorAxis();
        const double a = maj.length();
        const double bMin = a * e.ratio();
        const double x = rel.dot(maj.normalized()) / a;
        const double y = rel.dot(maj.perp().normalized()) / bMin;
        return x * x + y * y;
    };

    const Xform2d xforms[] = {
        Xform2d::rotation(0.7),                 // pure rotation
        Xform2d{1, 0, 0, -1, 0, 0},             // mirror about X
        Xform2d{2, 0, 0, 0.5, 3, -4},           // non-uniform scale + translate
        Xform2d::rotation(1.1).compose(Xform2d{-1.5, 0, 0, 0.8, 0, 0}), // mirror+rot+scale
    };
    for (const Xform2d& xf : xforms) {
        EllipseEntity e({10, 5}, {20, 0}, 0.5);
        e.transform(xf);
        for (int i = 0; i < 12; ++i) {
            const double t = 2.0 * M_PI * i / 12.0;
            EllipseEntity orig({10, 5}, {20, 0}, 0.5);
            const Vec2d mapped = xf.apply(orig.pointAt(t));
            CHECK(onEllipse(e, mapped) == Approx(1.0).margin(1e-6));
        }
        CHECK(e.ratio() > 0.0); // never negative after transform
    }
}

TEST_CASE("ellipse arc endpoints survive a mirror", "[entities][ellipse]")
{
    // Quarter ellipse, mirrored about X: the endpoint SET must be preserved.
    EllipseEntity e({0, 0}, {10, 0}, 0.5, 0.0, M_PI_2);
    const Vec2d p0 = e.pointAt(e.startParam());
    const Vec2d p1 = e.pointAt(e.endParam());
    const Xform2d mir{1, 0, 0, -1, 0, 0};
    e.transform(mir);
    const Vec2d q0 = e.pointAt(e.startParam());
    const Vec2d q1 = e.pointAt(e.endParam());
    const Vec2d m0 = mir.apply(p0);
    const Vec2d m1 = mir.apply(p1);
    const bool matched =
        (nearEqual(q0, m0, 1e-6) && nearEqual(q1, m1, 1e-6)) ||
        (nearEqual(q0, m1, 1e-6) && nearEqual(q1, m0, 1e-6));
    CHECK(matched);
}

TEST_CASE("spline de Boor evaluation hits control polygon ends", "[entities]")
{
    SplineEntity s;
    s.degree = 2;
    s.controlPoints = {{0, 0}, {5, 10}, {10, 0}};
    s.knots = {0, 0, 0, 1, 1, 1}; // clamped quadratic Bezier
    REQUIRE(nearEqual(s.evaluate(0.0), Vec2d{0, 0}, 1e-9));
    REQUIRE(nearEqual(s.evaluate(1.0), Vec2d{10, 0}, 1e-9));
    // Midpoint of a quadratic Bezier: (P0 + 2P1 + P2)/4 = (5, 5).
    REQUIRE(nearEqual(s.evaluate(0.5), Vec2d{5, 5}, 1e-9));
}

TEST_CASE("xline is infinite and clips to view box", "[entities]")
{
    XLineEntity xl({0, 0}, {1, 1});
    REQUIRE(xl.isInfinite());

    RenderContext ctx;
    ctx.viewBox = BBox2d({-100, -100}, {100, 100});
    PrimitiveList list;
    xl.buildPrimitives(ctx, list);
    REQUIRE(list.strokes.size() == 1);
    // Emitted segment spans well beyond the view box but is finite.
    REQUIRE(list.strokes[0].points[0].length() < 1e4);
}

TEST_CASE("all new types survive factory round-trip", "[entities]")
{
    for (const char* type : {"polyline", "ellipse", "spline", "point", "xline"}) {
        auto e = createEntityByType(QLatin1String(type));
        REQUIRE(e);
        REQUIRE(QLatin1String(e->typeName()) == QLatin1String(type));
        auto restored = entityFromJson(e->toJson());
        REQUIRE(restored);
        REQUIRE(json(*restored) == json(*e));
    }
}
