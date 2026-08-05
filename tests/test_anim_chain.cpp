// Chain v1 parsing (core/anim/Chain.h). The golden files under golden/anim/
// are verbatim copies of the GenMov3D reference instances (schemas/ +
// data/ of the GenMov3D project, 2026-08-05) so this repo tests against the
// real contract inputs without depending on a sibling checkout.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QJsonDocument>
#include <QJsonObject>

#include "anim/Chain.h"

using Catch::Approx;
using namespace viki::anim;

namespace {

QString goldenPath(const char* name)
{
    return QStringLiteral(VIKICAD_GOLDEN_DIR "/anim/")
           + QLatin1String(name);
}

ChainResult parse(const char* json,
                  std::optional<double> scaleOverrideM = {})
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    REQUIRE(doc.isObject());
    return chainFromJson(doc.object(), scaleOverrideM);
}

} // namespace

TEST_CASE("humanoid-12 golden chain parses", "[anim]")
{
    const ChainResult res = loadChainFile(goldenPath("humanoid-12.json"));
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    const Chain& c = res.chain;
    CHECK(c.id == QStringLiteral("humanoid-12"));
    CHECK(c.scaleMm == Approx(1750.0));
    REQUIRE(c.joints.size() == 13);

    // Root first, topologically ordered, free type.
    CHECK(c.joints[0].name == QStringLiteral("pelvis"));
    CHECK(c.joints[0].parent == -1);
    CHECK(c.joints[0].type == JointType::Free);
    CHECK(c.joints[0].attachMm.Z() == Approx(0.531 * 1750.0)); // 929.25 mm
    CHECK(res.warnings.isEmpty());

    // Fractions of scale_reference became millimetres.
    const int neck = c.indexOf(QStringLiteral("neck"));
    REQUIRE(neck >= 0);
    CHECK(c.joints[neck].attachMm.Z() == Approx(0.300 * 1750.0));
    CHECK(c.joints[neck].lengthMm == Approx(0.220 * 1750.0));
    const int ual = c.indexOf(QStringLiteral("upperarm_l"));
    REQUIRE(ual >= 0);
    CHECK(c.joints[ual].attachMm.X() == Approx(-0.100 * 1750.0));
    CHECK(c.joints[ual].restDirection.Z() == Approx(-1.0));

    // Every joint's parent sits earlier in the array.
    for (size_t i = 1; i < c.joints.size(); ++i) {
        CHECK(c.joints[i].parent >= 0);
        CHECK(c.joints[i].parent < static_cast<int>(i));
    }

    // Limits landed in radians: shin x stop is [-155, 0] deg (knee flexion
    // is negative X in the 2026-08-05 data revision).
    const int shin = c.indexOf(QStringLiteral("shin_l"));
    REQUIRE(shin >= 0);
    const auto lim = c.joints[shin].limits.value(QStringLiteral("x"));
    CHECK(lim.min == Approx(-155.0 * M_PI / 180.0));
    CHECK(lim.max == Approx(0.0));
}

TEST_CASE("avatar height_m override rescales the chain", "[anim]")
{
    const ChainResult res =
        loadChainFile(goldenPath("humanoid-12.json"), 2.0);
    REQUIRE(res.ok);
    const int neck = res.chain.indexOf(QStringLiteral("neck"));
    CHECK(res.chain.joints[neck].attachMm.Z() == Approx(600.0));
    CHECK(res.chain.scaleMm == Approx(2000.0));
}

TEST_CASE("chain joints are topologically re-ordered", "[anim]")
{
    // Child listed BEFORE its parent — parse must reorder, not reject.
    const ChainResult res = parse(R"({
        "id": "reorder", "schema_version": "1", "scale_reference": 1.0,
        "joints": [
            { "name": "tip", "parent": "arm", "type": "fixed",
              "attach": [0.1, 0, 0] },
            { "name": "arm", "parent": "base", "type": "revolute",
              "axis": [0, 0, 1], "attach": [0.02, 0.03, 0.04] },
            { "name": "base", "parent": null, "type": "free" }
        ]})");
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    REQUIRE(res.chain.joints.size() == 3);
    CHECK(res.chain.joints[0].name == QStringLiteral("base"));
    CHECK(res.chain.joints[1].name == QStringLiteral("arm"));
    CHECK(res.chain.joints[2].name == QStringLiteral("tip"));
    CHECK(res.chain.joints[1].parent == 0);
    CHECK(res.chain.joints[2].parent == 1);
}

TEST_CASE("chain validation refuses broken inputs", "[anim]")
{
    SECTION("wrong schema_version")
    {
        const ChainResult r = parse(R"({"id":"x","schema_version":"2",
            "joints":[{"name":"a","parent":null,"type":"free"}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("schema_version")));
    }
    SECTION("duplicate joint name")
    {
        const ChainResult r = parse(R"({"id":"x","schema_version":"1",
            "joints":[{"name":"a","parent":null,"type":"free"},
                      {"name":"a","parent":"a","type":"fixed"}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("duplicate")));
    }
    SECTION("unknown parent")
    {
        const ChainResult r = parse(R"({"id":"x","schema_version":"1",
            "joints":[{"name":"a","parent":null,"type":"free"},
                      {"name":"b","parent":"ghost","type":"fixed"}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("ghost")));
    }
    SECTION("two roots")
    {
        const ChainResult r = parse(R"({"id":"x","schema_version":"1",
            "joints":[{"name":"a","parent":null,"type":"free"},
                      {"name":"b","parent":null,"type":"free"}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("root")));
    }
    SECTION("parent cycle")
    {
        const ChainResult r = parse(R"({"id":"x","schema_version":"1",
            "joints":[{"name":"root","parent":null,"type":"free"},
                      {"name":"a","parent":"b","type":"fixed"},
                      {"name":"b","parent":"a","type":"fixed"}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("cycle")));
    }
    SECTION("revolute without axis")
    {
        const ChainResult r = parse(R"({"id":"x","schema_version":"1",
            "joints":[{"name":"a","parent":null,"type":"free"},
                      {"name":"b","parent":"a","type":"revolute"}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("axis")));
    }
    SECTION("length without rest_direction")
    {
        const ChainResult r = parse(R"({"id":"x","schema_version":"1",
            "joints":[{"name":"a","parent":null,"type":"free"},
                      {"name":"b","parent":"a","type":"fixed",
                       "length": 0.2}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("rest_direction")));
    }
    SECTION("free joint below the root")
    {
        const ChainResult r = parse(R"({"id":"x","schema_version":"1",
            "joints":[{"name":"a","parent":null,"type":"free"},
                      {"name":"b","parent":"a","type":"free"}]})");
        CHECK_FALSE(r.ok);
        CHECK(r.error.contains(QStringLiteral("free")));
    }
}

TEST_CASE("non-free root parses with a warning", "[anim]")
{
    const ChainResult r = parse(R"({"id":"x","schema_version":"1",
        "joints":[{"name":"frame","parent":null,"type":"fixed"},
                  {"name":"arm","parent":"frame","type":"revolute",
                   "axis":[0,1,0]}]})");
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings.first().contains(QStringLiteral("free")));
}
