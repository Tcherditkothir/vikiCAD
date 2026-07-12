#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopoDS.hxx>

#include "cmd/CommandProcessor.h"
#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"
#include "solid/SubShape.h"

// FILLETEDGES / CHAMFEREDGES / MATE / DRAFT — the remaining index-addressed
// parity commands. Edges and faces are referenced by the deterministic
// INSPECT indices (subshape::edgeAt/faceAt, TopExp order).

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

// INSPECT-index of the planar face whose centroid sits at height `z`.
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

// All planar-face indices whose centroid sits at height `z` (a box's four
// side faces when z is the mid-height).
std::vector<int> planarFaceIndicesAtZ(const TopoDS_Shape& shape, double z)
{
    std::vector<int> out;
    for (int i = 0; i < subshape::faceCount(shape); ++i) {
        const TopoDS_Face f = TopoDS::Face(subshape::faceAt(shape, i));
        const BRepAdaptor_Surface surf(f);
        if (surf.GetType() != GeomAbs_Plane)
            continue;
        GProp_GProps props;
        BRepGProp::SurfaceProperties(f, props);
        if (std::abs(props.CentreOfMass().Z() - z) < 1e-6)
            out.push_back(i);
    }
    return out;
}

double centroidZ(const TopoDS_Shape& shape)
{
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    return props.CentreOfMass().Z();
}

bool anyContains(const std::vector<QString>& msgs, const QString& needle)
{
    for (const QString& m : msgs)
        if (m.contains(needle))
            return true;
    return false;
}

// A 10x10x10 box solid built through commands (RECT profile id comes back).
EntityId makeBox10(Rig& rig, double x0, double y0)
{
    REQUIRE(rig.run(QStringLiteral("RECT %1,%2 %3,%4")
                        .arg(x0)
                        .arg(y0)
                        .arg(x0 + 10.0)
                        .arg(y0 + 10.0)));
    EntityId rectId = kInvalidEntityId;
    for (const EntityId id : rig.doc.drawOrder())
        rectId = std::max(rectId, id);
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 %1").arg(rectId)));
    const EntityId id = lastSolidId(rig.doc);
    REQUIRE(id != kInvalidEntityId);
    return id;
}

} // namespace

TEST_CASE("FILLETEDGES rounds one INSPECT-indexed edge — the exact "
          "quarter-cylinder complement — and undoes",
          "[subshape][filletedges][cmd]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    const EntityId id = makeBox10(rig, 0.0, 0.0);
    REQUIRE(volumeOf(solidById(rig.doc, id)->shape()) ==
            Approx(1000.0).epsilon(1e-6));

    // Every edge of the box is a straight 10 mm edge. Filleting ONE of them
    // by r=2 removes the complement of a quarter cylinder along its length:
    //   dV = (r^2 - pi*r^2/4) * L = (1 - pi/4) * 4 * 10
    const double r = 2.0, L = 10.0;
    const double complement = (1.0 - M_PI / 4.0) * r * r * L;
    REQUIRE(rig.run(QStringLiteral("FILLETEDGES 2 %1 0").arg(id)));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0 - complement).epsilon(1e-6));
    // One new cylindrical face appeared (the fillet), 6 -> 7 faces.
    CHECK(subshape::faceCount(solidById(rig.doc, id)->shape()) == 7);

    // Undoable.
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));
    CHECK(subshape::faceCount(solidById(rig.doc, id)->shape()) == 6);

    // Several edges at once shave more material than one.
    REQUIRE(rig.run(QStringLiteral("FILLETEDGES 2 %1 0 2").arg(id)));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) <
          1000.0 - complement + 1e-9);
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));

    // Out-of-range index answers with a diagnostic, not a crash.
    rig.ctx.clearMessages();
    rig.run(QStringLiteral("FILLETEDGES 2 %1 99").arg(id));
    CHECK(anyContains(rig.ctx.messages(), QStringLiteral("has no edge 99")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));
}

TEST_CASE("CHAMFEREDGES bevels one INSPECT-indexed edge — the exact "
          "triangular-prism cut — and undoes",
          "[subshape][chamferedges][cmd]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    const EntityId id = makeBox10(rig, 0.0, 0.0);

    // A d=2 chamfer on one 10 mm edge removes a triangular prism:
    //   dV = (d^2 / 2) * L = 2 * 10 = 20
    REQUIRE(rig.run(QStringLiteral("CHAMFEREDGES 2 %1 0").arg(id)));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(980.0).epsilon(1e-6));
    // One new planar face (the bevel), 6 -> 7 faces.
    CHECK(subshape::faceCount(solidById(rig.doc, id)->shape()) == 7);

    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));
}

TEST_CASE("MATE snaps a moving solid's face flat onto a fixed face by "
          "INSPECT indices, undoes",
          "[subshape][mate][cmd]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    const EntityId fixedId = makeBox10(rig, 0.0, 0.0);
    const EntityId movingId = makeBox10(rig, 30.0, 30.0);
    REQUIRE(fixedId != movingId);

    const int fixedTop =
        planarFaceIndexAtZ(solidById(rig.doc, fixedId)->shape(), 10.0);
    const int movingTop =
        planarFaceIndexAtZ(solidById(rig.doc, movingId)->shape(), 10.0);
    REQUIRE(fixedTop >= 0);
    REQUIRE(movingTop >= 0);
    const double zBefore = centroidZ(solidById(rig.doc, movingId)->shape());

    REQUIRE(rig.run(QStringLiteral("MATE %1 %2 %3 %4")
                        .arg(movingId)
                        .arg(movingTop)
                        .arg(fixedId)
                        .arg(fixedTop)));

    // applyTrsf preserves topology, so the same index still names the mated
    // face. The two faces are now coincident (distance ~0) and centred.
    const TopoDS_Shape movedFace =
        subshape::faceAt(solidById(rig.doc, movingId)->shape(), movingTop);
    const TopoDS_Shape fixedFace =
        subshape::faceAt(solidById(rig.doc, fixedId)->shape(), fixedTop);
    REQUIRE_FALSE(movedFace.IsNull());
    CHECK(solidops::minDistance(movedFace, fixedFace) ==
          Approx(0.0).margin(1e-6));
    // Outward normals opposed: top face flipped to face -Z, the moving box
    // now sits ON TOP of the fixed one (z 10..20, centroid z = 15).
    CHECK(centroidZ(solidById(rig.doc, movingId)->shape()) ==
          Approx(15.0).margin(1e-6));
    // Rigid motion: volume unchanged.
    CHECK(volumeOf(solidById(rig.doc, movingId)->shape()) ==
          Approx(1000.0).epsilon(1e-6));

    // Undo puts the moving solid back where it was.
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(centroidZ(solidById(rig.doc, movingId)->shape()) ==
          Approx(zBefore).margin(1e-6));

    // Out-of-range face index answers with a diagnostic.
    rig.ctx.clearMessages();
    rig.run(QStringLiteral("MATE %1 99 %2 %3")
                .arg(movingId)
                .arg(fixedId)
                .arg(fixedTop));
    CHECK(anyContains(rig.ctx.messages(), QStringLiteral("has no face 99")));

    // Non-planar faces are refused: mate a cylinder wall onto the box top.
    REQUIRE(rig.run(QStringLiteral("CIRCLE 60,60 5")));
    EntityId circleId = kInvalidEntityId;
    for (const EntityId eid : rig.doc.drawOrder())
        circleId = std::max(circleId, eid);
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 %1").arg(circleId)));
    const EntityId cylId = lastSolidId(rig.doc);
    int wallIdx = -1;
    for (int i = 0; i < subshape::faceCount(solidById(rig.doc, cylId)->shape());
         ++i) {
        const BRepAdaptor_Surface surf(TopoDS::Face(
            subshape::faceAt(solidById(rig.doc, cylId)->shape(), i)));
        if (surf.GetType() == GeomAbs_Cylinder) {
            wallIdx = i;
            break;
        }
    }
    REQUIRE(wallIdx >= 0);
    rig.ctx.clearMessages();
    rig.run(QStringLiteral("MATE %1 %2 %3 %4")
                .arg(cylId)
                .arg(wallIdx)
                .arg(fixedId)
                .arg(fixedTop));
    CHECK(anyContains(rig.ctx.messages(), QStringLiteral("PLANAR")));
}

TEST_CASE("DRAFT tapers the four side faces of a box by INSPECT indices "
          "(pull +Z, neutral plane at zMin), undoes",
          "[subshape][draft][cmd]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    const EntityId id = makeBox10(rig, 0.0, 0.0);
    REQUIRE(volumeOf(solidById(rig.doc, id)->shape()) ==
            Approx(1000.0).epsilon(1e-6));

    // The four side faces of the 10^3 box are the planar faces whose centroid
    // sits at mid-height z=5 (top is at 10, bottom at 0).
    const std::vector<int> sides =
        planarFaceIndicesAtZ(solidById(rig.doc, id)->shape(), 5.0);
    REQUIRE(sides.size() == 4);

    // Mirror the draftBoxSides test through the command: a 5 deg draft keeps
    // the volume in the sane band around (but not at) 1000.
    QString cmd = QStringLiteral("DRAFT 5 %1").arg(id);
    for (const int s : sides)
        cmd += QStringLiteral(" %1").arg(s);
    REQUIRE(rig.run(cmd));
    const double vol = volumeOf(solidById(rig.doc, id)->shape());
    CHECK(vol > 700.0);
    CHECK(vol < 1300.0);
    CHECK(vol != Approx(1000.0).epsilon(1e-3)); // the taper changed it

    // Undoable.
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));

    // A negative angle drafts the other way, still in-band, on the OTHER
    // side of 1000 from the positive draft.
    QString cmdNeg = QStringLiteral("DRAFT -5 %1").arg(id);
    for (const int s : sides)
        cmdNeg += QStringLiteral(" %1").arg(s);
    REQUIRE(rig.run(cmdNeg));
    const double volNeg = volumeOf(solidById(rig.doc, id)->shape());
    CHECK(volNeg > 700.0);
    CHECK(volNeg < 1300.0);
    CHECK((vol - 1000.0) * (volNeg - 1000.0) < 0.0);
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));

    // Guards: a zero angle is refused before any geometry runs.
    rig.ctx.clearMessages();
    rig.run(QStringLiteral("DRAFT 0 %1 %2").arg(id).arg(sides[0]));
    CHECK(anyContains(rig.ctx.messages(), QStringLiteral("non-zero")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));
    // Out-of-range face index answers with a diagnostic.
    rig.ctx.clearMessages();
    rig.run(QStringLiteral("DRAFT 5 %1 99").arg(id));
    CHECK(anyContains(rig.ctx.messages(), QStringLiteral("has no face 99")));
    CHECK(volumeOf(solidById(rig.doc, id)->shape()) ==
          Approx(1000.0).epsilon(1e-6));
}
