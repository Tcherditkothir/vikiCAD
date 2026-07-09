#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include <QTemporaryDir>

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>

#include "cmd/CommandProcessor.h"
#include "io/NativeStore.h"
#include "solid/FeatureTree.h"
#include "solid/SolidEntity.h"

using namespace viki;
using Catch::Approx;

namespace {
double volumeOf(const TopoDS_Shape& s)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(s, props);
    return props.Mass();
}

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    bool run(const QString& s) { return processor.submit(s, true).ok; }
};

SolidEntity* firstSolid(Document& doc)
{
    for (const EntityId id : doc.drawOrder())
        if (auto* s = dynamic_cast<SolidEntity*>(doc.entity(id)))
            return s;
    return nullptr;
}
} // namespace

TEST_CASE("FeatureTree replays sketch+extrude and reacts to a height edit")
{
    FeatureTree tree;

    // {sketch rect 20x20, extrude h=10} -> volume 4000.
    const int sketch = tree.addNode(FeatureNode::makeSketch(
        {SketchProfile::rectangle({0, 0}, {20, 20})}));
    const int extrude = tree.addNode(FeatureNode::makeExtrude(10.0, sketch));

    RegenResult r1 = tree.regenerate();
    REQUIRE(r1.ok);
    REQUIRE_FALSE(r1.shape.IsNull());
    REQUIRE(volumeOf(r1.shape) == Approx(4000.0).margin(1e-6));

    // Edit the extrude height to 25 -> regenerate -> volume 10000.
    REQUIRE(tree.setExtrudeHeight(extrude, 25.0));
    RegenResult r2 = tree.regenerate();
    REQUIRE(r2.ok);
    REQUIRE(volumeOf(r2.shape) == Approx(10000.0).margin(1e-6));

    // The original result is untouched: regenerate is a fresh replay.
    REQUIRE(volumeOf(r1.shape) == Approx(4000.0).margin(1e-6));
}

TEST_CASE("FeatureTree extrudes a circle profile")
{
    FeatureTree tree;
    tree.addNode(FeatureNode::makeSketch(
        {SketchProfile::circle({0, 0}, 5.0)}));
    tree.addNode(FeatureNode::makeExtrude(10.0)); // sketchIndex = -1 -> prior sketch

    RegenResult r = tree.regenerate();
    REQUIRE(r.ok);
    // pi * r^2 * h = pi * 25 * 10.
    REQUIRE(volumeOf(r.shape) == Approx(3.14159265358979 * 25.0 * 10.0).margin(1e-3));
}

TEST_CASE("FeatureTree Cut extrude subtracts from an earlier body")
{
    FeatureTree tree;

    // Block 20x20x10 = 4000.
    const int s1 = tree.addNode(FeatureNode::makeSketch(
        {SketchProfile::rectangle({0, 0}, {20, 20})}));
    const int block = tree.addNode(FeatureNode::makeExtrude(10.0, s1));

    // Cut a 4x4 pocket all the way through (height 10) -> remove 4*4*10 = 160.
    const int s2 = tree.addNode(FeatureNode::makeSketch(
        {SketchProfile::rectangle({8, 8}, {12, 12})}));
    tree.addNode(FeatureNode::makeExtrude(10.0, s2,
                                          solidops::ExtrudeMode::Cut, block));

    RegenResult r = tree.regenerate();
    REQUIRE(r.ok);
    REQUIRE(volumeOf(r.shape) == Approx(4000.0 - 160.0).margin(1e-6));
}

TEST_CASE("FeatureTree BaseShape+Hole replays and the diameter stays editable",
          "[featuretree][hole]")
{
    FeatureTree tree;
    tree.addNode(FeatureNode::makeBaseShape(
        BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape()));
    const int hole = tree.addNode(
        FeatureNode::makeHole(WorkPlane{}, {5, 5}, 4.0, 10.0, /*through=*/true));
    REQUIRE(tree.nodeCount() == 2);

    // 10^3 box minus a through bore d=4: 1000 - pi * 2^2 * 10.
    RegenResult r1 = tree.regenerate();
    REQUIRE(r1.ok);
    REQUIRE(volumeOf(r1.shape) == Approx(1000.0 - M_PI * 4.0 * 10.0).epsilon(1e-4));

    // Widen to d=6 -> 1000 - pi * 3^2 * 10. THE parametric payoff.
    REQUIRE(tree.setHoleDiameter(hole, 6.0));
    RegenResult r2 = tree.regenerate();
    REQUIRE(r2.ok);
    REQUIRE(volumeOf(r2.shape) == Approx(1000.0 - M_PI * 9.0 * 10.0).epsilon(1e-4));

    // Setter guards: wrong node kind / out of range.
    CHECK_FALSE(tree.setHoleDiameter(0, 5.0));   // node 0 is the BaseShape
    CHECK_FALSE(tree.setShellThickness(hole, 1.0));
    CHECK_FALSE(tree.setHoleDepth(99, 5.0));

    // A blind hole obeys setHoleDepth: d=4, depth 5 -> 1000 - pi*4*5.
    FeatureTree blind;
    blind.addNode(FeatureNode::makeBaseShape(
        BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape()));
    // Bore from the top face (z=10, normal +Z) down into the material.
    WorkPlane top;
    top.origin = gp_Pnt(0, 0, 10);
    const int bh = blind.addNode(
        FeatureNode::makeHole(top, {5, 5}, 4.0, 5.0, /*through=*/false));
    RegenResult rb = blind.regenerate();
    REQUIRE(rb.ok);
    REQUIRE(volumeOf(rb.shape) == Approx(1000.0 - M_PI * 4.0 * 5.0).epsilon(1e-4));
    REQUIRE(blind.setHoleDepth(bh, 8.0));
    rb = blind.regenerate();
    REQUIRE(rb.ok);
    REQUIRE(volumeOf(rb.shape) == Approx(1000.0 - M_PI * 4.0 * 8.0).epsilon(1e-4));
}

TEST_CASE("FeatureTree Shell node hollows and the thickness stays editable",
          "[featuretree][shell]")
{
    FeatureTree tree;
    tree.addNode(FeatureNode::makeBaseShape(
        BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape()));
    const int shell = tree.addNode(FeatureNode::makeShell(1.0));

    // 1mm wall all around a 10^3 box -> cavity 8^3: 1000 - 512 = 488.
    RegenResult r1 = tree.regenerate();
    REQUIRE(r1.ok);
    REQUIRE(volumeOf(r1.shape) == Approx(1000.0 - 512.0).epsilon(1e-3));

    // 2mm wall -> cavity 6^3: 1000 - 216 = 784.
    REQUIRE(tree.setShellThickness(shell, 2.0));
    RegenResult r2 = tree.regenerate();
    REQUIRE(r2.ok);
    REQUIRE(volumeOf(r2.shape) == Approx(1000.0 - 216.0).epsilon(1e-3));
}

TEST_CASE("FeatureTree JSON round-trip preserves every node kind",
          "[featuretree][json]")
{
    SECTION("BaseShape + Hole + Shell")
    {
        FeatureTree tree;
        tree.addNode(FeatureNode::makeBaseShape(
            BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape()));
        tree.addNode(
            FeatureNode::makeHole(WorkPlane{}, {5, 5}, 4.0, 10.0, true));
        tree.addNode(FeatureNode::makeShell(1.0));
        // Shell node round-trips even when appended after a hole; compare the
        // hole-only regen (drop the shell for the volume check) plus fields.
        const QJsonObject json = tree.toJson();

        FeatureTree back;
        REQUIRE(back.fromJson(json));
        REQUIRE(back.nodeCount() == 3);
        CHECK(back.nodeAt(0).kind == FeatureKind::BaseShape);
        CHECK(back.nodeAt(1).kind == FeatureKind::Hole);
        CHECK(back.nodeAt(1).diameter == Approx(4.0));
        CHECK(back.nodeAt(1).through);
        CHECK(back.nodeAt(2).kind == FeatureKind::Shell);
        CHECK(back.nodeAt(2).thickness == Approx(1.0));

        // The restored BaseShape brep is bit-usable: regen of {base, hole}
        // (node 2 removed on a copy) matches the original volumes.
        FeatureTree holed;
        holed.addNode(back.nodeAt(0));
        holed.addNode(back.nodeAt(1));
        const RegenResult a = holed.regenerate();
        REQUIRE(a.ok);
        CHECK(volumeOf(a.shape) ==
              Approx(1000.0 - M_PI * 4.0 * 10.0).epsilon(1e-4));
    }

    SECTION("Sketch profiles + Extrude modes (cut pocket)")
    {
        FeatureTree tree;
        const int s1 = tree.addNode(FeatureNode::makeSketch(
            {SketchProfile::rectangle({0, 0}, {20, 20})}));
        const int block = tree.addNode(FeatureNode::makeExtrude(10.0, s1));
        const int s2 = tree.addNode(FeatureNode::makeSketch(
            {SketchProfile::rectangle({8, 8}, {12, 12})}));
        tree.addNode(FeatureNode::makeExtrude(10.0, s2,
                                              solidops::ExtrudeMode::Cut, block));
        const double vol = volumeOf(tree.regenerate().shape);
        REQUIRE(vol == Approx(4000.0 - 160.0).margin(1e-6));

        FeatureTree back;
        REQUIRE(back.fromJson(tree.toJson()));
        REQUIRE(back.nodeCount() == 4);
        CHECK(back.nodeAt(3).mode == solidops::ExtrudeMode::Cut);
        CHECK(back.nodeAt(3).targetIndex == block);
        const RegenResult r = back.regenerate();
        REQUIRE(r.ok);
        CHECK(volumeOf(r.shape) == Approx(vol).margin(1e-6));
    }

    SECTION("circle and polygon profiles survive the trip")
    {
        FeatureTree tree;
        tree.addNode(FeatureNode::makeSketch(
            {SketchProfile::circle({1, 2}, 5.0),
             SketchProfile::polygonFrom({{0, 0}, {30, 0}, {0, 30}})}));

        FeatureTree back;
        REQUIRE(back.fromJson(tree.toJson()));
        REQUIRE(back.nodeCount() == 1);
        const FeatureNode& sk = back.nodeAt(0);
        REQUIRE(sk.profiles.size() == 2);
        CHECK(sk.profiles[0].kind == ProfileKind::Circle);
        CHECK(sk.profiles[0].circleRadius == Approx(5.0));
        CHECK(sk.profiles[0].circleCenter.x == Approx(1.0));
        REQUIRE(sk.profiles[1].kind == ProfileKind::Polygon);
        CHECK(sk.profiles[1].polygon.size() == 3);
        CHECK(sk.profiles[1].polygon[1].x == Approx(30.0));
    }

    SECTION("malformed input is rejected")
    {
        FeatureTree back;
        CHECK_FALSE(back.fromJson(QJsonObject{})); // no "nodes"
        CHECK(back.nodeCount() == 0);
    }
}

TEST_CASE("HOLE command records an editable history that survives save/load",
          "[featuretree][hole][vkd]")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("features.vkd"));
    const double drilled = 4000.0 - M_PI * 4.0 * 10.0;

    {
        Rig rig;
        REQUIRE(rig.run(QStringLiteral("WORKPLANE XY"))); // pin the plane
        REQUIRE(rig.run(QStringLiteral("RECT 0,0 20,20")));
        REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1"))); // solid id 2, 4000
        REQUIRE(rig.run(QStringLiteral("HOLE 4 T 10,10 2")));

        SolidEntity* s = firstSolid(rig.doc);
        REQUIRE(s);
        CHECK(volumeOf(s->shape()) == Approx(drilled).epsilon(1e-4));
        // The command recorded {BaseShape(target brep), Hole}.
        REQUIRE(s->features);
        REQUIRE(s->features->nodeCount() == 2);
        CHECK(s->features->nodeAt(0).kind == FeatureKind::BaseShape);
        CHECK(s->features->nodeAt(1).kind == FeatureKind::Hole);

        QString error;
        REQUIRE(NativeStore::save(rig.doc, path, error));
    }

    // Reload: the history rode the .vkd, the volume is unchanged, and the
    // hole is STILL editable (the whole point of the wiring).
    QString error;
    const auto doc = NativeStore::load(path, error);
    REQUIRE(doc);
    SolidEntity* s = firstSolid(*doc);
    REQUIRE(s);
    CHECK(volumeOf(s->shape()) == Approx(drilled).epsilon(1e-4));
    REQUIRE(s->features);
    REQUIRE(s->features->nodeCount() == 2);

    REQUIRE(s->features->setHoleDiameter(1, 6.0));
    REQUIRE(s->regenerateFeatures());
    CHECK(volumeOf(s->shape()) ==
          Approx(4000.0 - M_PI * 9.0 * 10.0).epsilon(1e-4));
}

TEST_CASE("SHELL command records Shell history on the modified solid",
          "[featuretree][shell]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1"))); // solid id 2, vol 1000
    REQUIRE(rig.run(QStringLiteral("SHELL 1 2")));

    SolidEntity* s = firstSolid(rig.doc);
    REQUIRE(s);
    CHECK(volumeOf(s->shape()) == Approx(1000.0 - 512.0).epsilon(1e-3));
    REQUIRE(s->features);
    REQUIRE(s->features->nodeCount() == 2);
    CHECK(s->features->nodeAt(0).kind == FeatureKind::BaseShape);
    CHECK(s->features->nodeAt(1).kind == FeatureKind::Shell);

    // Thicken the wall afterwards — still parametric.
    REQUIRE(s->features->setShellThickness(1, 2.0));
    REQUIRE(s->regenerateFeatures());
    CHECK(volumeOf(s->shape()) == Approx(1000.0 - 216.0).epsilon(1e-3));

    // Undo restores the featureless pre-shell solid (JSON snapshot path).
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    SolidEntity* pre = firstSolid(rig.doc);
    REQUIRE(pre);
    CHECK(volumeOf(pre->shape()) == Approx(1000.0).epsilon(1e-6));
    CHECK_FALSE(pre->features);
}
