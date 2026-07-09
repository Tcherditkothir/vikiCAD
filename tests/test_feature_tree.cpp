#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include "solid/FeatureTree.h"

using namespace viki;
using Catch::Approx;

namespace {
double volumeOf(const TopoDS_Shape& s)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(s, props);
    return props.Mass();
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
