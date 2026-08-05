// Forward kinematics on a small crane mechanism: free root, revolute boom,
// prismatic carriage, fixed hook. All attach points are deliberately
// asymmetric (no two components equal) so a wrong-axis rotation cannot land
// on the expected numbers by coincidence (DEVLOG 2026-08-03).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QJsonDocument>

#include "anim/AnimClip.h"
#include "anim/Chain.h"
#include "anim/ForwardKinematics.h"

using Catch::Approx;
using namespace viki::anim;

namespace {

// Rest layout (mm, world): base (30,20,50) -> boom (40,40,180) ->
// carriage (110,40,180) -> hook (120,40,175).
const char* kCraneJson = R"({
    "id": "crane", "schema_version": "1", "scale_reference": 1.0,
    "joints": [
        { "name": "base", "parent": null, "type": "free",
          "attach": [0.03, 0.02, 0.05], "length": 0.13,
          "rest_direction": [0, 0, 1] },
        { "name": "boom", "parent": "base", "type": "revolute",
          "axis": [0, 1, 0], "attach": [0.01, 0.02, 0.13],
          "length": 0.4, "rest_direction": [1, 0, 0],
          "limits_deg": { "x": [-90, 90] } },
        { "name": "carriage", "parent": "boom", "type": "prismatic",
          "axis": [1, 0, 0], "attach": [0.07, 0, 0],
          "length": 0.02, "rest_direction": [1, 0, 0] },
        { "name": "hook", "parent": "carriage", "type": "fixed",
          "attach": [0.01, 0, -0.005], "length": 0.03,
          "rest_direction": [0, 0, -1] }
    ]})";

Chain crane()
{
    const QJsonDocument doc = QJsonDocument::fromJson(kCraneJson);
    REQUIRE(doc.isObject());
    const ChainResult res = chainFromJson(doc.object());
    INFO(res.error.toStdString());
    REQUIRE(res.ok);
    return res.chain;
}

ClipResult craneClip(const Chain& chain, const char* json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    REQUIRE(doc.isObject());
    return clipFromJson(doc.object(), chain);
}

void checkPnt(const gp_Pnt& p, double x, double y, double z)
{
    CHECK(p.X() == Approx(x).margin(1e-9));
    CHECK(p.Y() == Approx(y).margin(1e-9));
    CHECK(p.Z() == Approx(z).margin(1e-9));
}

} // namespace

TEST_CASE("rest pose stacks the attach points", "[anim]")
{
    const Chain chain = crane();
    const ClipResult clip = craneClip(chain, R"({
        "id":"rest","schema_version":"1","chain":"crane","fps":24,
        "keyframes":[{"t":0}]})");
    REQUIRE(clip.ok);
    const auto world =
        worldTransforms(chain, clip.clip.sampleAt(0.0));
    checkPnt(jointOrigin(world, 0), 30, 20, 50);
    checkPnt(jointOrigin(world, 1), 40, 40, 180);
    checkPnt(jointOrigin(world, 2), 110, 40, 180);
    checkPnt(jointOrigin(world, 3), 120, 40, 175);
    // Segment ends: boom reaches (440,40,180), hook hangs to (120,40,145).
    checkPnt(segmentEnd(chain, world, 1), 440, 40, 180);
    checkPnt(segmentEnd(chain, world, 3), 120, 40, 145);
}

TEST_CASE("revolute rotates its whole subtree about the local axis",
          "[anim]")
{
    const Chain chain = crane();
    const ClipResult clip = craneClip(chain, R"({
        "id":"rev","schema_version":"1","chain":"crane","fps":24,
        "keyframes":[{"t":0,"joints":{"boom":90}}]})");
    REQUIRE(clip.ok);
    const auto world =
        worldTransforms(chain, clip.clip.sampleAt(0.0));
    // Ry(+90): (x,y,z) -> (z, y, -x), applied at the boom origin
    // (40,40,180). The boom origin itself must not move.
    checkPnt(jointOrigin(world, 1), 40, 40, 180);
    // Carriage local attach (70,0,0) -> (0,0,-70).
    checkPnt(jointOrigin(world, 2), 40, 40, 110);
    // Hook attach (10,0,-5) -> (-5,0,-10).
    checkPnt(jointOrigin(world, 3), 35, 40, 100);
    // Boom tip: (400,0,0) -> (0,0,-400).
    checkPnt(segmentEnd(chain, world, 1), 40, 40, -220);
}

TEST_CASE("prismatic slides along its axis in metres", "[anim]")
{
    const Chain chain = crane();
    const ClipResult clip = craneClip(chain, R"({
        "id":"slide","schema_version":"1","chain":"crane","fps":24,
        "keyframes":[{"t":0,"joints":{"carriage":0.05}}]})");
    REQUIRE(clip.ok);
    const auto world =
        worldTransforms(chain, clip.clip.sampleAt(0.0));
    checkPnt(jointOrigin(world, 2), 160, 40, 180); // +50 mm along +X
    checkPnt(jointOrigin(world, 3), 170, 40, 175); // hook follows
}

TEST_CASE("root channels move and turn the whole chain", "[anim]")
{
    const Chain chain = crane();
    SECTION("root_rot spins around Z")
    {
        const ClipResult clip = craneClip(chain, R"({
            "id":"spin","schema_version":"1","chain":"crane","fps":24,
            "keyframes":[{"t":0,"root_rot":[0,0,90]}]})");
        REQUIRE(clip.ok);
        const auto world =
            worldTransforms(chain, clip.clip.sampleAt(0.0));
        // Base keeps its rest position; Rz(+90): (x,y) -> (-y,x) on the
        // boom attach (10,20,130).
        checkPnt(jointOrigin(world, 0), 30, 20, 50);
        checkPnt(jointOrigin(world, 1), 10, 30, 180);
    }
    SECTION("root_pos is absolute, not additive")
    {
        const ClipResult clip = craneClip(chain, R"({
            "id":"move","schema_version":"1","chain":"crane","fps":24,
            "keyframes":[{"t":0,"root_pos":[0.001,0.002,0.003]}]})");
        REQUIRE(clip.ok);
        const auto world =
            worldTransforms(chain, clip.clip.sampleAt(0.0));
        checkPnt(jointOrigin(world, 0), 1, 2, 3);
        checkPnt(jointOrigin(world, 1), 11, 22, 133);
    }
}

TEST_CASE("scalar interpolation is linear between keyframes", "[anim]")
{
    const Chain chain = crane();
    const ClipResult clip = craneClip(chain, R"({
        "id":"lerp","schema_version":"1","chain":"crane","fps":24,
        "keyframes":[
            {"t":0,"joints":{"carriage":0.01}},
            {"t":2,"joints":{"carriage":0.09}}
        ]})");
    REQUIRE(clip.ok);
    const int carriage = chain.indexOf(QStringLiteral("carriage"));
    // Quarter of the way: 0.01 + 0.25 x 0.08 = 0.03 m = 30 mm.
    const PoseSample s = clip.clip.sampleAt(0.5);
    CHECK(s.values[carriage].scalar == Approx(30.0).margin(1e-9));
}

TEST_CASE("revolute stop violations warn in degrees", "[anim]")
{
    const Chain chain = crane();
    const ClipResult clip = craneClip(chain, R"({
        "id":"over","schema_version":"1","chain":"crane","fps":24,
        "keyframes":[{"t":0,"joints":{"boom":135}}]})");
    REQUIRE(clip.ok);
    REQUIRE(clip.warnings.size() == 1);
    CHECK(clip.warnings.first().contains(QStringLiteral("boom.x")));
    CHECK(clip.warnings.first().contains(QStringLiteral("135.0 deg")));
}

TEST_CASE("a typed root joint animates (lever, robot base)", "[anim]")
{
    // Review 2026-08-05: the root's own channel used to be silently
    // overwritten by root_pos/root_rot — a single-revolute lever never
    // moved. The composition is place ∘ placeRot ∘ slide ∘ typedRot.
    const char* leverJson = R"({
        "id": "lever", "schema_version": "1", "scale_reference": 1.0,
        "joints": [
            { "name": "lever", "parent": null, "type": "revolute",
              "axis": [0, 1, 0], "attach": [0.01, 0.02, 0.03],
              "length": 0.1, "rest_direction": [1, 0, 0] }
        ]})";
    const ChainResult chain =
        chainFromJson(QJsonDocument::fromJson(leverJson).object());
    INFO(chain.error.toStdString());
    REQUIRE(chain.ok);
    // Non-free root: informational warning, even on a 1-joint chain.
    REQUIRE(chain.warnings.size() == 1);

    SECTION("the lever's own channel moves it")
    {
        const ClipResult clip = craneClip(chain.chain, R"({
            "id":"swing","schema_version":"1","chain":"lever","fps":24,
            "keyframes":[{"t":0,"joints":{"lever":90}}]})");
        REQUIRE(clip.ok);
        const auto world =
            worldTransforms(chain.chain, clip.clip.sampleAt(0.0));
        // Ry(+90): tip (100,0,0) -> (0,0,-100), from origin (10,20,30).
        checkPnt(jointOrigin(world, 0), 10, 20, 30);
        checkPnt(segmentEnd(chain.chain, world, 0), 10, 20, -70);
    }
    SECTION("root_rot composes BEFORE the lever's own rotation")
    {
        const ClipResult clip = craneClip(chain.chain, R"({
            "id":"swing","schema_version":"1","chain":"lever","fps":24,
            "keyframes":[{"t":0,"root_rot":[90,0,0],
                          "joints":{"lever":90}}]})");
        REQUIRE(clip.ok);
        const auto world =
            worldTransforms(chain.chain, clip.clip.sampleAt(0.0));
        // R = Rx(90) ∘ Ry(90): tip (100,0,0) -Ry-> (0,0,-100)
        // -Rx-> (0,100,0). Reversed composition would give (10,20,-70).
        checkPnt(segmentEnd(chain.chain, world, 0), 10, 120, 30);
    }
}

TEST_CASE("fixed joints refuse pose values", "[anim]")
{
    const Chain chain = crane();
    const ClipResult clip = craneClip(chain, R"({
        "id":"bad","schema_version":"1","chain":"crane","fps":24,
        "keyframes":[{"t":0,"joints":{"hook":5}}]})");
    CHECK_FALSE(clip.ok);
    CHECK(clip.error.contains(QStringLiteral("fixed")));
}
