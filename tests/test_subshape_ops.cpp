#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopoDS.hxx>

#include "cmd/CommandProcessor.h"
#include "solid/SolidEntity.h"
#include "solid/SubShape.h"

// PUSHPULL / SHELLOPEN / SPLITFACE — the headless, index-addressed twins of
// the GUI-only direct-modeling ops. Faces are referenced by the deterministic
// INSPECT indices (subshape::faceAt, TopExp order).

using namespace viki;
using Catch::Approx;

namespace {

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    bool run(const QString& s) { return processor.submit(s, true).ok; }
};

double volumeOf(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.Mass();
}

std::vector<const SolidEntity*> allSolids(const Document& doc)
{
    std::vector<const SolidEntity*> out;
    for (const EntityId id : doc.drawOrder())
        if (const auto* s = dynamic_cast<const SolidEntity*>(doc.entity(id)))
            out.push_back(s);
    return out;
}

EntityId lastSolidId(const Document& doc)
{
    EntityId last = kInvalidEntityId;
    for (const EntityId id : doc.drawOrder())
        if (dynamic_cast<const SolidEntity*>(doc.entity(id)))
            last = std::max(last, id);
    return last;
}

const SolidEntity* solidById(const Document& doc, EntityId id)
{
    return dynamic_cast<const SolidEntity*>(doc.entity(id));
}

// INSPECT-index of the planar face whose centroid sits at height `z`
// (a box's top/bottom face). -1 when absent.
int planarFaceIndexAtZ(const TopoDS_Shape& shape, double z)
{
    for (int i = 0; i < subshape::faceCount(shape); ++i) {
        const TopoDS_Face f = TopoDS::Face(subshape::faceAt(shape, i));
        const BRepAdaptor_Surface surf(f);
        if (surf.GetType() != GeomAbs_Plane)
            continue;
        GProp_GProps props;
        BRepGProp::SurfaceProperties(f, props);
        if (std::abs(props.CentreOfMass().Z() - z) < 1e-6)
            return i;
    }
    return -1;
}

// INSPECT-index of the first CYLINDRICAL face (a bore/boss lateral wall).
int cylinderFaceIndex(const TopoDS_Shape& shape)
{
    for (int i = 0; i < subshape::faceCount(shape); ++i) {
        const BRepAdaptor_Surface surf(
            TopoDS::Face(subshape::faceAt(shape, i)));
        if (surf.GetType() == GeomAbs_Cylinder)
            return i;
    }
    return -1;
}

bool anyContains(const std::vector<QString>& msgs, const QString& needle)
{
    for (const QString& m : msgs)
        if (m.contains(needle))
            return true;
    return false;
}

} // namespace

TEST_CASE("PUSHPULL bosses and pockets a box face by INSPECT index, undoes",
          "[subshape][pushpull][cmd]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1")));
    const EntityId id = lastSolidId(rig.doc);
    REQUIRE(id != kInvalidEntityId);
    REQUIRE(volumeOf(solidById(rig.doc, id)->shape()) ==
            Approx(1000.0).epsilon(1e-6));

    const int top = planarFaceIndexAtZ(solidById(rig.doc, id)->shape(), 10.0);
    REQUIRE(top >= 0);

    // +5 on the top face = a boss: 1000 + 10*10*5 = 1500.
    REQUIRE(rig.run(QStringLiteral("PUSHPULL 5 %1 %2").arg(top).arg(id)));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1500.0).epsilon(1e-6));

    // Undo restores the original body.
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));

    // -4 on the same face = a pocket: 1000 - 10*10*4 = 600.
    REQUIRE(rig.run(QStringLiteral("PUSHPULL -4 %1 %2").arg(top).arg(id)));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(600.0).epsilon(1e-6));
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));

    // Out-of-range index answers with a diagnostic, not a crash.
    rig.ctx.clearMessages();
    rig.run(QStringLiteral("PUSHPULL 5 99 %1").arg(id));
    CHECK(anyContains(rig.ctx.messages(), QStringLiteral("has no face 99")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));
}

TEST_CASE("PUSHPULL refuses a curved face — the PLANAR guard flows through",
          "[subshape][pushpull][cmd]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    REQUIRE(rig.run(QStringLiteral("CIRCLE 0,0 5")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1")));
    const EntityId id = lastSolidId(rig.doc);
    REQUIRE(id != kInvalidEntityId);
    const double before = volumeOf(solidById(rig.doc, id)->shape());
    REQUIRE(before == Approx(M_PI * 25.0 * 10.0).epsilon(1e-4));

    const int wall = cylinderFaceIndex(solidById(rig.doc, id)->shape());
    REQUIRE(wall >= 0);

    rig.ctx.clearMessages();
    rig.run(QStringLiteral("PUSHPULL 5 %1 %2").arg(wall).arg(id));
    // The solidops rejection message reaches the agent verbatim...
    CHECK(anyContains(rig.ctx.messages(), QStringLiteral("PLANAR")));
    // ...and the part did NOT vanish or change.
    REQUIRE(solidById(rig.doc, id));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(before).epsilon(1e-9));
}

TEST_CASE("SHELLOPEN opens top+bottom of a 10^3 box into a 360 mm^3 tube",
          "[subshape][shell][cmd]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1")));
    const EntityId id = lastSolidId(rig.doc);
    REQUIRE(id != kInvalidEntityId);

    const TopoDS_Shape& shape = solidById(rig.doc, id)->shape();
    const int top = planarFaceIndexAtZ(shape, 10.0);
    const int bottom = planarFaceIndexAtZ(shape, 0.0);
    REQUIRE(top >= 0);
    REQUIRE(bottom >= 0);
    REQUIRE(top != bottom);

    // The multi-face shellSolid overload through the COMMAND path:
    // 10*10*10 - 8*8*10 = 360 (a square tube, walls 1 mm).
    REQUIRE(rig.run(
        QStringLiteral("SHELLOPEN 1 %1 %2 %3").arg(id).arg(top).arg(bottom)));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(360.0).epsilon(1e-4));

    // Undo restores the full body.
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));

    // Single open face = an open-top box: 1000 - 8*8*9 = 424.
    REQUIRE(rig.run(QStringLiteral("SHELLOPEN 1 %1 %2").arg(id).arg(top)));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(424.0).epsilon(1e-4));
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));

    // Missing face index cancels with a diagnostic; the solid is untouched.
    rig.ctx.clearMessages();
    rig.run(QStringLiteral("SHELLOPEN 1 %1").arg(id));
    CHECK(anyContains(rig.ctx.messages(),
                      QStringLiteral("at least one face index")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));
}

TEST_CASE("SPLITFACE cuts a box with a cylinder wall; pieces inherit fields; "
          "undo restores",
          "[subshape][split][cmd]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1")));
    const EntityId boxId = lastSolidId(rig.doc);
    REQUIRE(boxId != kInvalidEntityId);

    // Tool: a cylinder r=3 through the middle of the box, extended past both
    // ends (z -5..15) so its LATERAL face cuts the full height cleanly.
    REQUIRE(rig.run(QStringLiteral("WORKPLANE OFFSET -5")));
    REQUIRE(rig.run(QStringLiteral("CIRCLE 5,5 3")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 20 3")));
    const EntityId toolId = lastSolidId(rig.doc);
    REQUIRE(toolId != boxId);
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));

    const int wall = cylinderFaceIndex(solidById(rig.doc, toolId)->shape());
    REQUIRE(wall >= 0);

    // Tag the target so field inheritance is observable on the pieces.
    rig.doc.beginTransaction(QStringLiteral("tag"));
    if (auto* s = dynamic_cast<SolidEntity*>(rig.doc.beginModify(boxId))) {
        s->component = QStringLiteral("PartA");
        s->transparency = 0.25;
        rig.doc.endModify(boxId);
    }
    rig.doc.commitTransaction();

    REQUIRE(allSolids(rig.doc).size() == 2);
    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("SPLITFACE %1 %2 %3")
                        .arg(toolId)
                        .arg(wall)
                        .arg(boxId)));
    for (const QString& m : rig.ctx.messages())
        UNSCOPED_INFO(m.toStdString());

    // Target replaced by 2 pieces; the tool cylinder is untouched.
    const auto solids = allSolids(rig.doc);
    REQUIRE(solids.size() == 3);
    CHECK(solidById(rig.doc, boxId) == nullptr);
    REQUIRE(solidById(rig.doc, toolId));

    std::vector<double> pieceVols;
    for (const auto* s : solids) {
        if (s == solidById(rig.doc, toolId))
            continue;
        pieceVols.push_back(volumeOf(s->shape()));
        // The SPLIT replacement block: pieces inherit the target's fields.
        CHECK(s->component == QStringLiteral("PartA"));
        CHECK(s->transparency == Approx(0.25));
    }
    REQUIRE(pieceVols.size() == 2);
    std::sort(pieceVols.begin(), pieceVols.end());
    // Inner core = pi*3^2*10; outer = the rest; the cut conserves material.
    CHECK(pieceVols[0] == Approx(M_PI * 9.0 * 10.0).epsilon(1e-4));
    CHECK(pieceVols[0] + pieceVols[1] == Approx(1000.0).epsilon(1e-6));

    // One UNDO restores the single tagged box.
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    REQUIRE(allSolids(rig.doc).size() == 2);
    const auto* restored = solidById(rig.doc, boxId);
    REQUIRE(restored);
    CHECK(volumeOf(restored->shape()) == Approx(1000.0).epsilon(1e-6));
    CHECK(restored->component == QStringLiteral("PartA"));

    // Guards: tool == target, and a face that misses the target.
    rig.ctx.clearMessages();
    rig.run(QStringLiteral("SPLITFACE %1 %2 %3")
                .arg(toolId)
                .arg(wall)
                .arg(toolId));
    CHECK(anyContains(rig.ctx.messages(),
                      QStringLiteral("target must be another solid")));
}

