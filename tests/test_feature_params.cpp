#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>

#include "cmd/CommandProcessor.h"
#include "solid/FeatureParams.h"
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

TEST_CASE("featureparams::list enumerates the editable node parameters",
          "[featureparams]")
{
    FeatureTree tree;
    tree.addNode(FeatureNode::makeBaseShape(
        BRepPrimAPI_MakeBox(20.0, 20.0, 10.0).Shape()));
    tree.addNode(
        FeatureNode::makeHole(WorkPlane{}, {10, 10}, 4.0, 5.0, /*through=*/false));
    tree.addNode(FeatureNode::makeShell(1.0));

    // BaseShape contributes nothing; blind hole exposes diameter+depth.
    auto params = featureparams::list(tree);
    REQUIRE(params.size() == 5); // diameter, depth, center x/y, thickness
    CHECK(params[0].label == QStringLiteral("hole 1: diameter"));
    CHECK(params[0].nodeIndex == 1);
    CHECK(params[0].value == Approx(4.0));
    CHECK(params[1].label == QStringLiteral("hole 1: depth"));
    CHECK(params[1].value == Approx(5.0));
    CHECK(params[2].label == QStringLiteral("hole 1: center x"));
    CHECK(params[2].value == Approx(10.0));
    CHECK(params[3].label == QStringLiteral("hole 1: center y"));
    CHECK(params[3].value == Approx(10.0));
    CHECK(params[4].label == QStringLiteral("shell 2: thickness"));
    CHECK(params[4].nodeIndex == 2);
    CHECK(params[4].value == Approx(1.0));

    // A through hole hides its (ignored) depth; the centre stays editable.
    tree.nodeAt(1).through = true;
    params = featureparams::list(tree);
    REQUIRE(params.size() == 4);
    CHECK(params[0].label == QStringLiteral("hole 1: diameter"));
    CHECK(params[1].label == QStringLiteral("hole 1: center x"));
    CHECK(params[3].label == QStringLiteral("shell 2: thickness"));

    // set() guards: kind mismatch, unknown name, non-positive value.
    CHECK_FALSE(featureparams::set(tree, 0, QStringLiteral("diameter"), 6.0));
    CHECK_FALSE(featureparams::set(tree, 1, QStringLiteral("bogus"), 6.0));
    CHECK_FALSE(featureparams::set(tree, 1, QStringLiteral("diameter"), -2.0));
    CHECK_FALSE(featureparams::set(tree, 1, QStringLiteral("diameter"), 0.0));
    CHECK(tree.nodeAt(1).diameter == Approx(4.0)); // untouched by the misses

    CHECK(featureparams::set(tree, 1, QStringLiteral("diameter"), 6.0));
    CHECK(tree.nodeAt(1).diameter == Approx(6.0));
    CHECK(featureparams::set(tree, 2, QStringLiteral("thickness"), 2.0));
    CHECK(tree.nodeAt(2).thickness == Approx(2.0));
}

TEST_CASE("Properties-panel core path: a placed hole stays editable, with undo",
          "[featureparams][hole]")
{
    // HOLE end-to-end, exactly as a user would place it.
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 20,20")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1"))); // 20x20x10 -> vol 4000
    REQUIRE(rig.run(QStringLiteral("HOLE 4 T 10,10 2")));

    SolidEntity* s = firstSolid(rig.doc);
    REQUIRE(s);
    REQUIRE(s->features);
    const double before = 4000.0 - M_PI * 4.0 * 10.0;
    CHECK(volumeOf(s->shape()) == Approx(before).epsilon(1e-4));
    const EntityId id = s->id();

    // The panel lists "hole 1: diameter" plus the centre (through bore -> no
    // depth row).
    const auto params = featureparams::list(*s->features);
    REQUIRE(params.size() == 3);
    CHECK(params[0].label == QStringLiteral("hole 1: diameter"));
    CHECK(params[0].value == Approx(4.0));
    CHECK(params[1].label == QStringLiteral("hole 1: center x"));
    CHECK(params[2].label == QStringLiteral("hole 1: center y"));

    // The panel's edit path: setter + regenerate inside ONE transaction.
    rig.doc.beginTransaction(QStringLiteral("FEATURE EDIT"));
    {
        Entity* e = rig.doc.beginModify(id);
        REQUIRE(e);
        auto* solid = dynamic_cast<SolidEntity*>(e);
        REQUIRE(solid);
        REQUIRE(solid->features);
        REQUIRE(featureparams::set(*solid->features, params[0].nodeIndex,
                                   params[0].name, 6.0));
        REQUIRE(solid->regenerateFeatures());
        rig.doc.endModify(id);
    }
    rig.doc.commitTransaction();

    // Volume follows the wider bore: 4000 - pi * 3^2 * 10.
    s = firstSolid(rig.doc);
    REQUIRE(s);
    CHECK(volumeOf(s->shape()) ==
          Approx(4000.0 - M_PI * 9.0 * 10.0).epsilon(1e-4));
    CHECK(s->features->nodeAt(1).diameter == Approx(6.0));

    // ONE undo restores both the previous volume and the previous parameter.
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    s = firstSolid(rig.doc);
    REQUIRE(s);
    REQUIRE(s->features);
    CHECK(volumeOf(s->shape()) == Approx(before).epsilon(1e-4));
    CHECK(s->features->nodeAt(1).diameter == Approx(4.0));
}

TEST_CASE("Feature edit revert path: failed regeneration keeps the old shape",
          "[featureparams][shell]")
{
    // A shell wall far thicker than the body cannot regenerate; the panel
    // logic reverts the parameter and the solid keeps its previous shape.
    SolidEntity solid;
    auto tree = std::make_unique<FeatureTree>();
    tree->addNode(FeatureNode::makeBaseShape(
        BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape()));
    const int shell = tree->addNode(FeatureNode::makeShell(1.0));
    solid.features = std::move(tree);
    REQUIRE(solid.regenerateFeatures());
    const double good = volumeOf(solid.shape());
    CHECK(good == Approx(1000.0 - 512.0).epsilon(1e-3));

    REQUIRE(featureparams::set(*solid.features, shell,
                               QStringLiteral("thickness"), 500.0));
    if (!solid.regenerateFeatures()) {
        // regenerateFeatures leaves the shape untouched on failure...
        CHECK(volumeOf(solid.shape()) == Approx(good).epsilon(1e-6));
        // ...and the panel puts the old value back; replay works again.
        REQUIRE(featureparams::set(*solid.features, shell,
                                   QStringLiteral("thickness"), 1.0));
        REQUIRE(solid.regenerateFeatures());
        CHECK(volumeOf(solid.shape()) == Approx(good).epsilon(1e-3));
    }
    // (If OCCT ever accepts the absurd wall, the tree is still consistent —
    // nothing to revert. The assertions above only bind on failure.)
}
