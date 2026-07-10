#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QTemporaryDir>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include "cmd/CommandProcessor.h"
#include "io/NativeStore.h"
#include "io/StepIo.h"
#include "script/ScriptRunner.h"
#include "solid/SolidEntity.h"

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

const SolidEntity* firstSolid(const Document& doc)
{
    for (const EntityId id : doc.drawOrder())
        if (const auto* s = dynamic_cast<const SolidEntity*>(doc.entity(id)))
            return s;
    return nullptr;
}
} // namespace

TEST_CASE("EXTRUDE a rectangle into a box with exact volume", "[m7]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 40,30")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 20 1")));
    REQUIRE(rig.doc.entityCount() == 1); // profile consumed
    const SolidEntity* solid = firstSolid(rig.doc);
    REQUIRE(solid);
    REQUIRE(volumeOf(solid->shape()) == Approx(40.0 * 30.0 * 20.0).epsilon(1e-6));
    // 2D footprint matches the profile.
    REQUIRE(solid->bounds().width() == Approx(40.0));

    // Undo restores the profile and removes the solid (BREP in the journal).
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    REQUIRE(firstSolid(rig.doc) == nullptr);
    REQUIRE(rig.doc.entityCount() == 1);
    REQUIRE(rig.run(QStringLiteral("REDO")));
    const SolidEntity* again = firstSolid(rig.doc);
    REQUIRE(again);
    REQUIRE(volumeOf(again->shape()) == Approx(24000.0).epsilon(1e-6));
}

TEST_CASE("EXTRUDE a circle into a cylinder", "[m7]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("CIRCLE 0,0 10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 50 1")));
    const SolidEntity* solid = firstSolid(rig.doc);
    REQUIRE(solid);
    REQUIRE(volumeOf(solid->shape()) == Approx(M_PI * 100.0 * 50.0).epsilon(1e-4));
}

TEST_CASE("EXTRUDE a closed line chain (profile chaining)", "[m7]")
{
    Rig rig;
    // Triangle from three separate lines.
    REQUIRE(rig.run(QStringLiteral("LINE 0,0 30,0")));
    REQUIRE(rig.run(QStringLiteral("LINE 30,0 0,30")));
    REQUIRE(rig.run(QStringLiteral("LINE 0,30 0,0")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1 2 3")));
    const SolidEntity* solid = firstSolid(rig.doc);
    REQUIRE(solid);
    REQUIRE(volumeOf(solid->shape()) == Approx(0.5 * 30 * 30 * 10).epsilon(1e-6));
}

TEST_CASE("REVOLVE a profile around an axis", "[m7]")
{
    Rig rig;
    // 10x10 square, 5 units from the Y axis, revolved 360°:
    // volume = 2*pi*R*A with R=10 (centroid), A=100 → Pappus.
    REQUIRE(rig.run(QStringLiteral("RECT 5,0 15,10")));
    REQUIRE(rig.run(QStringLiteral("REVOLVE 360 0,0 0,10 1")));
    const SolidEntity* solid = firstSolid(rig.doc);
    REQUIRE(solid);
    REQUIRE(volumeOf(solid->shape()) == Approx(2 * M_PI * 10.0 * 100.0).epsilon(1e-4));
}

TEST_CASE("boolean SUBTRACT drills a hole", "[m7]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 40,40")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1")));
    REQUIRE(rig.run(QStringLiteral("CIRCLE 20,20 5")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 3")));
    // ids: solid1 = 2, solid2 = 4
    REQUIRE(rig.run(QStringLiteral("SUBTRACT 2 4")));
    const SolidEntity* solid = firstSolid(rig.doc);
    REQUIRE(solid);
    const double expected = 40.0 * 40.0 * 10.0 - M_PI * 25.0 * 10.0;
    REQUIRE(volumeOf(solid->shape()) == Approx(expected).epsilon(1e-4));
}

TEST_CASE("WORKPLANE OFFSET extrudes at altitude", "[m7]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE OFFSET 100")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 5 1")));
    const SolidEntity* solid = firstSolid(rig.doc);
    REQUIRE(solid);
    // Solid sits at z=100..105; footprint unchanged.
    REQUIRE(solid->bounds().width() == Approx(10.0));
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid->shape(), props);
    REQUIRE(props.CentreOfMass().Z() == Approx(102.5).epsilon(1e-6));
    // Reset for other tests sharing the registry keyed by document.
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
}

TEST_CASE("solids persist in .vkd byte-exactly through save/load", "[m7]")
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("solid.vkd"));
    double volume = 0;
    {
        Rig rig;
        REQUIRE(rig.run(QStringLiteral("CIRCLE 0,0 8")));
        REQUIRE(rig.run(QStringLiteral("EXTRUDE 30 1")));
        volume = volumeOf(firstSolid(rig.doc)->shape());
        QString error;
        REQUIRE(NativeStore::save(rig.doc, path, error));
    }
    QString error;
    const auto doc = NativeStore::load(path, error);
    REQUIRE(doc);
    const SolidEntity* solid = firstSolid(*doc);
    REQUIRE(solid);
    REQUIRE(volumeOf(solid->shape()) == Approx(volume).epsilon(1e-9));
}

TEST_CASE("STEP export/import round-trip with sidecar notes", "[m7][step]")
{
    QTemporaryDir dir;
    const QString step = dir.filePath(QStringLiteral("part.step"));

    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 20,20")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 20 1")));
    const auto rs = runScript(rig.processor,
                              QStringLiteral("NOTE 25,25 tolerance H7 sur l'alesage\n"));
    REQUIRE(rs.ok);

    const StepResult ex = exportStep(rig.doc, step);
    REQUIRE(ex.ok);
    REQUIRE(ex.solids == 1);
    REQUIRE(ex.notes == 1);
    REQUIRE(QFile::exists(step + QStringLiteral(".vikinotes.json")));

    std::unique_ptr<Document> back;
    const StepResult im = importStep(step, back);
    REQUIRE(im.ok);
    REQUIRE(im.solids == 1);
    REQUIRE(im.notes == 1);
    const SolidEntity* solid = firstSolid(*back);
    REQUIRE(solid);
    REQUIRE(volumeOf(solid->shape()) == Approx(8000.0).epsilon(1e-4));
}

TEST_CASE("FILLET3D and CHAMFER3D round all edges", "[m8]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 20,20")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 20 1")));
    const double before = volumeOf(firstSolid(rig.doc)->shape());
    REQUIRE(rig.run(QStringLiteral("FILLET3D 2 2")));
    const double after = volumeOf(firstSolid(rig.doc)->shape());
    // Fillets shave material off a convex box.
    REQUIRE(after < before);
    REQUIRE(after > before * 0.8);

    // Undo restores the sharp box exactly (BREP delta in the journal).
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    REQUIRE(volumeOf(firstSolid(rig.doc)->shape()) == Approx(before).epsilon(1e-9));

    REQUIRE(rig.run(QStringLiteral("CHAMFER3D 1 2")));
    REQUIRE(volumeOf(firstSolid(rig.doc)->shape()) < before);
}

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include "solid/SolidOps.h"

TEST_CASE("Push/Pull a face grows (boss) and shrinks (pocket) the solid", "[m7][pushpull]")
{
    using namespace viki;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    REQUIRE(volumeOf(box) == Approx(1000.0).epsilon(1e-9));

    // First face of the box (area 100).
    TopExp_Explorer exp(box, TopAbs_FACE);
    REQUIRE(exp.More());
    const TopoDS_Shape face = exp.Current();

    // Boss: push +5 along the outward normal -> +100*5 = 500.
    const auto boss = solidops::pushPullFace(box, face, 5.0);
    REQUIRE(boss.ok);
    CHECK(volumeOf(boss.shape) == Approx(1500.0).epsilon(1e-6));

    // Pocket: pull -3 into the material -> -100*3 = 300.
    const auto pocket = solidops::pushPullFace(box, face, -3.0);
    REQUIRE(pocket.ok);
    CHECK(volumeOf(pocket.shape) == Approx(700.0).epsilon(1e-6));

    // Guards.
    CHECK_FALSE(solidops::pushPullFace(box, face, 0.0).ok);
    CHECK_FALSE(solidops::pushPullFace(box, box, 5.0).ok); // not a face
}

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <TopExp_Explorer.hxx>

TEST_CASE("EXTRUDE follows a non-XY work plane", "[m7][workplane]")
{
    Rig rig;
    // A circle r5 in sketch coords, extruded on the YZ plane (normal +X).
    REQUIRE(rig.run(QStringLiteral("CIRCLE 0,0 5")));
    documentWorkplane(rig.doc) =
        WorkPlane{gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0), gp_Dir(0, 1, 0)};
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1")));
    const SolidEntity* solid = firstSolid(rig.doc);
    REQUIRE(solid);
    // Same volume as any r5/height10 cylinder, but now the axis is +X.
    CHECK(volumeOf(solid->shape()) == Approx(M_PI * 25.0 * 10.0).epsilon(1e-4));
    Bnd_Box box;
    BRepBndLib::Add(solid->shape(), box);
    double xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    CHECK(xmax - xmin == Approx(10.0).margin(0.2)); // extruded along X
    CHECK(ymax - ymin == Approx(10.0).margin(0.2)); // circle diameter
    CHECK(zmax - zmin == Approx(10.0).margin(0.2));
}

TEST_CASE("planeFromFace extracts a planar face's frame", "[m7][workplane]")
{
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    bool foundTop = false;
    for (TopExp_Explorer e(box, TopAbs_FACE); e.More(); e.Next()) {
        const auto wp = solidops::planeFromFace(e.Current());
        REQUIRE(wp.has_value());
        // Every box face normal is axis-aligned and unit length.
        const gp_Dir n = wp->normal;
        CHECK(std::abs(n.X()) + std::abs(n.Y()) + std::abs(n.Z()) ==
              Approx(1.0).margin(1e-9));
        if (std::abs(n.Z() - 1.0) < 1e-6)
            foundTop = true;
    }
    CHECK(foundTop); // the +Z top face is present
}

TEST_CASE("makeHole drills a through and a blind hole", "[m7][hole]")
{
    using namespace viki;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    REQUIRE(volumeOf(box) == Approx(1000.0).epsilon(1e-9));

    // Through hole d=4 on the bottom XY plane, centred under the footprint:
    // pierces the full 10mm height, removing pi*2^2*10.
    const WorkPlane xy{gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)};
    const auto through = solidops::makeHole(box, xy, Vec2d{5, 5}, 4.0, 0.0, true);
    REQUIRE(through.ok);
    CHECK(volumeOf(through.shape) ==
          Approx(1000.0 - M_PI * 4.0 * 10.0).epsilon(1e-4));
    // The hole is interior: the bounding box is unchanged (nothing pierced the
    // outer walls).
    Bnd_Box bb;
    BRepBndLib::Add(through.shape, bb);
    double xmin, ymin, zmin, xmax, ymax, zmax;
    bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    CHECK(xmax - xmin == Approx(10.0).margin(1e-6));
    CHECK(zmax - zmin == Approx(10.0).margin(1e-6));

    // Blind hole from the TOP face (normal +Z points out of the material, so the
    // bore runs -Z into the body): depth 3 removes pi*2^2*3.
    const WorkPlane top{gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)};
    const auto blind = solidops::makeHole(box, top, Vec2d{5, 5}, 4.0, 3.0, false);
    REQUIRE(blind.ok);
    CHECK(volumeOf(blind.shape) ==
          Approx(1000.0 - M_PI * 4.0 * 3.0).epsilon(1e-4));

    // Guards.
    CHECK_FALSE(solidops::makeHole(box, xy, Vec2d{5, 5}, 0.0, 5.0, false).ok);
    CHECK_FALSE(solidops::makeHole(box, xy, Vec2d{5, 5}, 4.0, 0.0, false).ok);
    CHECK_FALSE(solidops::makeHole(TopoDS_Shape(), xy, Vec2d{5, 5}, 4.0, 5.0, true).ok);
}

TEST_CASE("extrudeWires Symmetric doubles the height about the plane", "[m7][extrude-modes]")
{
    using namespace viki;
    // A r5 circle profile on the XY plane, extruded symmetrically (total 10).
    const WorkPlane xy{gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)};
    // Build the wire via a document + wiresFromEntities for parity with EXTRUDE.
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("CIRCLE 0,0 5")));
    const auto wires =
        solidops::wiresFromEntities(rig.doc, {EntityId(1)}, xy);
    REQUIRE(wires.ok);

    // One-sided height 10 spans z 0..10.
    const auto oneSided =
        solidops::extrudeWires(wires.wires, 10.0, xy, solidops::ExtrudeMode::NewBody);
    REQUIRE(oneSided.ok);
    {
        Bnd_Box bb;
        BRepBndLib::Add(oneSided.shape, bb);
        double xmn, ymn, zmn, xmx, ymx, zmx;
        bb.Get(xmn, ymn, zmn, xmx, ymx, zmx);
        CHECK(zmn == Approx(0.0).margin(1e-6));
        CHECK(zmx == Approx(10.0).margin(1e-6));
    }

    // Symmetric total height 10 spans z -5..+5 (same volume, centred).
    const auto sym =
        solidops::extrudeWires(wires.wires, 10.0, xy, solidops::ExtrudeMode::Symmetric);
    REQUIRE(sym.ok);
    CHECK(volumeOf(sym.shape) == Approx(M_PI * 25.0 * 10.0).epsilon(1e-4));
    Bnd_Box bb;
    BRepBndLib::Add(sym.shape, bb);
    double xmn, ymn, zmn, xmx, ymx, zmx;
    bb.Get(xmn, ymn, zmn, xmx, ymx, zmx);
    CHECK(zmn == Approx(-5.0).margin(1e-6));
    CHECK(zmx == Approx(5.0).margin(1e-6));
    CHECK(zmx - zmn == Approx(10.0).margin(1e-6)); // full height about the plane
}

TEST_CASE("extrudeWires Cut removes the prism from a target box", "[m7][extrude-modes]")
{
    using namespace viki;
    // Target: 40x40x10 box built from a RECT extrusion.
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(40.0, 40.0, 10.0).Shape();
    REQUIRE(volumeOf(box) == Approx(16000.0).epsilon(1e-9));

    // A r5 circle centred at (20,20), extruded 10 up as a Cut tool.
    const WorkPlane xy{gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)};
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("CIRCLE 20,20 5")));
    const auto wires = solidops::wiresFromEntities(rig.doc, {EntityId(1)}, xy);
    REQUIRE(wires.ok);

    const auto cut =
        solidops::extrudeWires(wires.wires, 10.0, xy, solidops::ExtrudeMode::Cut, box);
    REQUIRE(cut.ok);
    CHECK(volumeOf(cut.shape) == Approx(16000.0 - M_PI * 25.0 * 10.0).epsilon(1e-4));

    // Join adds the prism back (fusing overlapping volume yields the box again).
    const auto join =
        solidops::extrudeWires(wires.wires, 10.0, xy, solidops::ExtrudeMode::Join, box);
    REQUIRE(join.ok);
    CHECK(volumeOf(join.shape) == Approx(16000.0).epsilon(1e-4));

    // Guard: Join/Cut without a target fails.
    CHECK_FALSE(solidops::extrudeWires(wires.wires, 10.0, xy,
                                       solidops::ExtrudeMode::Cut).ok);
}

TEST_CASE("EXTRUDE command Cut mode subtracts a prism from a target solid",
          "[m7][extrude-modes]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    // Target box solid (id 2), then a circle profile (id 3).
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 40,40")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1"))); // solid id 2
    REQUIRE(volumeOf(firstSolid(rig.doc)->shape()) == Approx(16000.0).epsilon(1e-6));
    REQUIRE(rig.run(QStringLiteral("CIRCLE 20,20 5"))); // profile id 3

    // Pre-select the profile so the trailing id on the line is the target.
    rig.selection.add(3);
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 CUT 2")));
    const SolidEntity* holed = firstSolid(rig.doc);
    REQUIRE(holed);
    REQUIRE(rig.doc.entityCount() == 1); // profile + target consumed, one solid
    CHECK(volumeOf(holed->shape()) ==
          Approx(16000.0 - M_PI * 25.0 * 10.0).epsilon(1e-4));

    // Undo restores the intact box and the profile.
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(volumeOf(firstSolid(rig.doc)->shape()) == Approx(16000.0).epsilon(1e-6));
}

TEST_CASE("EXTRUDE command Symmetric mode centres the solid on the work plane",
          "[m7][extrude-modes]")
{
    Rig rig;
    // Pin the work plane to XY (the per-document registry can carry a stale
    // plane from a sibling test that reuses this Document's address).
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 20 SYMMETRIC 1")));
    const SolidEntity* solid = firstSolid(rig.doc);
    REQUIRE(solid);
    CHECK(volumeOf(solid->shape()) == Approx(10.0 * 10.0 * 20.0).epsilon(1e-6));
    Bnd_Box bb;
    BRepBndLib::Add(solid->shape(), bb);
    double xmn, ymn, zmn, xmx, ymx, zmx;
    bb.Get(xmn, ymn, zmn, xmx, ymx, zmx);
    CHECK(zmn == Approx(-10.0).margin(1e-6)); // centred: -10..+10
    CHECK(zmx == Approx(10.0).margin(1e-6));

    // Legacy form still works: EXTRUDE height id defaults to New (one-sided).
    Rig rig2;
    REQUIRE(rig2.run(QStringLiteral("WORKPLANE XY")));
    REQUIRE(rig2.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig2.run(QStringLiteral("EXTRUDE 20 1")));
    const SolidEntity* s2 = firstSolid(rig2.doc);
    REQUIRE(s2);
    Bnd_Box bb2;
    BRepBndLib::Add(s2->shape(), bb2);
    double a, b, zmn2, c, d, zmx2;
    bb2.Get(a, b, zmn2, c, d, zmx2);
    CHECK(zmn2 == Approx(0.0).margin(1e-6)); // one-sided: 0..20
    CHECK(zmx2 == Approx(20.0).margin(1e-6));
}

TEST_CASE("HOLE command bores a through hole and undoes", "[m7][hole]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 20,20")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1"))); // solid id 2
    REQUIRE(volumeOf(firstSolid(rig.doc)->shape()) == Approx(4000.0).epsilon(1e-6));

    // HOLE diameter T(hrough) center-point solid-id — numeric params first.
    REQUIRE(rig.run(QStringLiteral("HOLE 4 T 10,10 2")));
    const SolidEntity* holed = firstSolid(rig.doc);
    REQUIRE(holed);
    REQUIRE(rig.doc.entityCount() == 1); // old solid consumed, one solid remains
    CHECK(volumeOf(holed->shape()) ==
          Approx(4000.0 - M_PI * 4.0 * 10.0).epsilon(1e-4));

    // Undo restores the un-drilled solid (BREP in the journal).
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    REQUIRE(volumeOf(firstSolid(rig.doc)->shape()) == Approx(4000.0).epsilon(1e-6));
}

TEST_CASE("SHELL hollows a solid to a wall thickness", "[m8][shell]")
{
    using namespace viki;
    // Direct op: a 10^3 box shelled by 1mm leaves a 1mm wall all around; the
    // hollow removes an inner 8^3 cavity -> ~1000 - 512 = 488.
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    REQUIRE(volumeOf(box) == Approx(1000.0).epsilon(1e-9));

    const auto shelled = solidops::shellSolid(box, 1.0);
    REQUIRE(shelled.ok);
    REQUIRE_FALSE(shelled.shape.IsNull());
    const double vol = volumeOf(shelled.shape);
    CHECK(vol > 0.0);
    CHECK(vol < 1000.0);                              // clearly less than solid
    CHECK(vol == Approx(1000.0 - 8.0 * 8.0 * 8.0).epsilon(1e-3)); // ~488

    // Guards.
    CHECK_FALSE(solidops::shellSolid(box, 0.0).ok);       // zero thickness
    CHECK_FALSE(solidops::shellSolid(TopoDS_Shape(), 1.0).ok); // null solid

    // SHELL command: thickness first, then pick the solid; mutates in place.
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1"))); // solid id 2, vol 1000
    REQUIRE(volumeOf(firstSolid(rig.doc)->shape()) == Approx(1000.0).epsilon(1e-6));

    REQUIRE(rig.run(QStringLiteral("SHELL 1 2")));
    const SolidEntity* hollow = firstSolid(rig.doc);
    REQUIRE(hollow);
    REQUIRE(rig.doc.entityCount() == 1); // shelled in place
    const double cmdVol = volumeOf(hollow->shape());
    CHECK(cmdVol > 0.0);
    CHECK(cmdVol < 1000.0);

    // Undo restores the solid box (BREP in the journal).
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    CHECK(volumeOf(firstSolid(rig.doc)->shape()) == Approx(1000.0).epsilon(1e-6));
}

TEST_CASE("filletEdges/chamferEdges round SPECIFIC edges of a solid", "[m8][edges]")
{
    using namespace viki;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    REQUIRE(volumeOf(box) == Approx(1000.0).epsilon(1e-9));

    // A box has 12 geometric edges. TopExp lists each face's boundary, so the
    // raw explorer count is higher (edges are shared between adjacent faces).
    // Filleting ALL of them by r=1 shaves a small amount off every convex edge
    // — the result stays a valid solid, strictly less than the original 1000
    // but well above zero (the spec's 0 < newVol < 1000 assertion).
    int edgeCount = 0;
    for (TopExp_Explorer e(box, TopAbs_EDGE); e.More(); e.Next())
        ++edgeCount;
    REQUIRE(edgeCount >= 12);

    const auto allFillet = solidops::filletFirstNEdges(box, 0, 1.0);
    REQUIRE(allFillet.ok);
    REQUIRE_FALSE(allFillet.shape.IsNull());
    const double allVol = volumeOf(allFillet.shape);
    CHECK(allVol > 0.0);
    CHECK(allVol < 1000.0);

    // Filleting only the FIRST edge removes less material than filleting all 12.
    const auto oneFillet = solidops::filletFirstNEdges(box, 1, 1.0);
    REQUIRE(oneFillet.ok);
    const double oneVol = volumeOf(oneFillet.shape);
    CHECK(oneVol < 1000.0);
    CHECK(oneVol > allVol); // one edge shaves less than twelve

    // filletEdges with an explicit edge vector (as the GUI picker would supply).
    std::vector<TopoDS_Shape> picked;
    for (TopExp_Explorer e(box, TopAbs_EDGE); e.More() && picked.size() < 3; e.Next())
        picked.push_back(e.Current());
    const auto some = solidops::filletEdges(box, picked, 1.0);
    REQUIRE(some.ok);
    CHECK(volumeOf(some.shape) < 1000.0);

    // Chamfer the first edge too (bevels rather than rounds).
    const auto oneCham = solidops::chamferFirstNEdges(box, 1, 1.0);
    REQUIRE(oneCham.ok);
    CHECK(volumeOf(oneCham.shape) < 1000.0);
    CHECK(volumeOf(oneCham.shape) > 0.0);

    // Guards: null solid, empty edge set, non-positive size.
    CHECK_FALSE(solidops::filletEdges(TopoDS_Shape(), picked, 1.0).ok);
    CHECK_FALSE(solidops::filletEdges(box, {}, 1.0).ok);
    CHECK_FALSE(solidops::filletFirstNEdges(box, 1, 0.0).ok);
    CHECK_FALSE(solidops::chamferEdges(box, {}, 1.0).ok);
}

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepExtrema_DistShapeShape.hxx>

namespace {
// Pick the face of `shape` whose outward normal is closest to `want`.
TopoDS_Shape faceByNormal(const TopoDS_Shape& shape, const gp_Dir& want)
{
    TopoDS_Shape best;
    double bestDot = -2.0;
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
        const auto wp = solidops::planeFromFace(e.Current());
        if (!wp)
            continue;
        const double d = gp_Vec(wp->normal).Dot(gp_Vec(want));
        if (d > bestDot) {
            bestDot = d;
            best = e.Current();
        }
    }
    return best;
}
} // namespace

TEST_CASE("mateTransform snaps a moving face flat onto a fixed face", "[m7][mate]")
{
    using namespace viki;
    // Fixed box at the origin; its +Z face sits at z=10.
    const TopoDS_Shape fixed = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    // Moving box translated to (30,30,30) so it starts well apart; its +Z
    // face sits at z=40.
    gp_Trsf place;
    place.SetTranslation(gp_Vec(30.0, 30.0, 30.0));
    const TopoDS_Shape moving = BRepBuilderAPI_Transform(
        BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape(), place).Shape();

    const TopoDS_Shape faceA = faceByNormal(moving, gp_Dir(0, 0, 1)); // moving +Z
    const TopoDS_Shape faceB = faceByNormal(fixed, gp_Dir(0, 0, 1));  // fixed +Z
    REQUIRE(!faceA.IsNull());
    REQUIRE(!faceB.IsNull());

    const auto trsf = solidops::mateTransform(faceA, faceB);
    REQUIRE(trsf.has_value());

    // Apply through SolidEntity::applyTrsf, the documented entry point.
    SolidEntity ent(moving);
    ent.applyTrsf(*trsf);
    const TopoDS_Shape movedFaceA = faceByNormal(ent.shape(), gp_Dir(0, 0, -1));
    // After the mate faceA's outward normal points opposite faceB's (+Z), so it
    // now faces -Z; grab the moved solid's -Z-facing face and check coincidence.
    REQUIRE(!movedFaceA.IsNull());

    // 1) The mated faces are coincident: distance ~0.
    BRepExtrema_DistShapeShape dist(movedFaceA, faceB);
    REQUIRE(dist.IsDone());
    CHECK(dist.Value() == Approx(0.0).margin(1e-6));

    // 2) Their outward normals are opposed.
    const auto wpMoved = solidops::planeFromFace(movedFaceA);
    const auto wpFixed = solidops::planeFromFace(faceB);
    REQUIRE(wpMoved.has_value());
    REQUIRE(wpFixed.has_value());
    const double dot = gp_Vec(wpMoved->normal).Dot(gp_Vec(wpFixed->normal));
    CHECK(dot == Approx(-1.0).margin(1e-6));

    // Guards: non-planar / null faces yield nullopt.
    CHECK_FALSE(solidops::mateTransform(fixed, faceB).has_value());  // solid, not a face
    CHECK_FALSE(solidops::mateTransform(faceA, TopoDS_Shape()).has_value());
}

TEST_CASE("minDistance between two boxes 5mm apart along X", "[m7][measure3d]")
{
    using namespace viki;
    // Box A occupies x in [0,10]; box B is translated +15 in X so it occupies
    // x in [15,25]. The nearest faces sit at x=10 and x=15 → gap = 5mm.
    const TopoDS_Shape a = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    gp_Trsf shift;
    shift.SetTranslation(gp_Vec(15.0, 0.0, 0.0));
    const TopoDS_Shape b =
        BRepBuilderAPI_Transform(BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape(),
                                 shift, true)
            .Shape();

    CHECK(solidops::minDistance(a, b) == Approx(5.0).margin(1e-9));
    // Symmetric.
    CHECK(solidops::minDistance(b, a) == Approx(5.0).margin(1e-9));
    // A shape against itself touches → 0.
    CHECK(solidops::minDistance(a, a) == Approx(0.0).margin(1e-9));
    // Null shape → -1 failure sentinel.
    CHECK(solidops::minDistance(a, TopoDS_Shape()) == Approx(-1.0));
}

TEST_CASE("MEASURE3D command reports two boxes 5mm apart", "[m7][measure3d]")
{
    using namespace viki;
    Rig rig;
    gp_Trsf shift;
    shift.SetTranslation(gp_Vec(15.0, 0.0, 0.0));
    rig.doc.beginTransaction(QStringLiteral("setup"));
    // Box A: 10x10x10 at origin.
    const EntityId ia = rig.doc.addEntity(std::make_unique<SolidEntity>(
        BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape()));
    // Box B: shifted +15 in X → 5mm gap.
    const EntityId ib = rig.doc.addEntity(std::make_unique<SolidEntity>(
        BRepBuilderAPI_Transform(BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape(),
                                 shift, true)
            .Shape()));
    rig.doc.commitTransaction();

    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("MEASURE3D %1 %2").arg(ia).arg(ib)));
    REQUIRE_FALSE(rig.ctx.messages().empty());
    const QString msg = rig.ctx.messages().back();
    CHECK(msg.contains(QStringLiteral("min distance")));
    // The reported number is 5.
    CHECK(msg.contains(QStringLiteral("5")));
}

#include <gp_Dir.hxx>
#include <gp_Pln.hxx>

TEST_CASE("DRAFT tapers the side faces of a box", "[m8][draft]")
{
    using namespace viki;
    // A 10^3 box, pull +Z, neutral plane at z=0 (its bottom face). Drafting the
    // four side faces by a few degrees tips them inward/outward, moving the
    // volume away from 1000 while keeping a valid, sane solid.
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    REQUIRE(volumeOf(box) == Approx(1000.0).epsilon(1e-9));

    const gp_Dir pull(0.0, 0.0, 1.0);
    const gp_Pln neutral(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0));

    const auto drafted = solidops::draftBoxSides(box, pull, neutral, 5.0);
    REQUIRE(drafted.ok);
    REQUIRE_FALSE(drafted.shape.IsNull());
    const double vol = volumeOf(drafted.shape);
    CHECK(vol > 0.0);
    CHECK(vol != Approx(1000.0).epsilon(1e-3)); // taper changed the volume
    CHECK(vol > 700.0);
    CHECK(vol < 1300.0);

    // A negative angle drafts the other way; still a valid solid in-band.
    const auto draftedNeg = solidops::draftBoxSides(box, pull, neutral, -5.0);
    REQUIRE(draftedNeg.ok);
    const double volNeg = volumeOf(draftedNeg.shape);
    CHECK(volNeg > 700.0);
    CHECK(volNeg < 1300.0);
    CHECK(volNeg != Approx(1000.0).epsilon(1e-3));

    // Guards.
    CHECK_FALSE(solidops::draftBoxSides(TopoDS_Shape(), pull, neutral, 5.0).ok);
    CHECK_FALSE(solidops::draftFaces(box, {}, pull, neutral, 5.0).ok);
}

#include <algorithm>

#include "render/Primitives.h"

TEST_CASE("solid renders its real 2D silhouette, no placeholder label", "[m7][silhouette]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 20,20")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1")));
    const SolidEntity* solid = firstSolid(rig.doc);
    REQUIRE(solid);

    RenderContext ctx;
    PrimitiveList prims;
    solid->buildPrimitives(ctx, prims);

    // The generic "[3D WxHxD]" placeholder label is gone.
    CHECK(prims.texts.empty());
    // A real silhouette is drawn as several stroke segments...
    REQUIRE(prims.strokes.size() >= 4);
    // ...aligned 1:1 to world XY (NOT mirrored): the union bbox is the 20x20
    // footprint at the solid's true position.
    Vec2d lo{1e9, 1e9}, hi{-1e9, -1e9};
    for (const auto& s : prims.strokes)
        for (const auto& p : s.points) {
            lo.x = std::min(lo.x, p.x);
            lo.y = std::min(lo.y, p.y);
            hi.x = std::max(hi.x, p.x);
            hi.y = std::max(hi.y, p.y);
        }
    CHECK(lo.x == Approx(0.0).margin(1e-6));
    CHECK(lo.y == Approx(0.0).margin(1e-6));
    CHECK(hi.x == Approx(20.0).margin(1e-6));
    CHECK(hi.y == Approx(20.0).margin(1e-6));
}

TEST_CASE("projectToPlane2d round-trips with planePoint3d", "[m7][plane2d]")
{
    // A tilted plane: origin off-axis, normal +X, xDir +Y (the YZ plane).
    const WorkPlane plane{gp_Pnt(5, -2, 7), gp_Dir(1, 0, 0), gp_Dir(0, 1, 0)};
    const Vec2d uv{12.5, -3.75};
    const gp_Pnt p = solidops::planePoint3d(uv, plane);
    const Vec2d back = solidops::projectToPlane2d(p, plane);
    CHECK(back.x == Approx(uv.x).margin(1e-9));
    CHECK(back.y == Approx(uv.y).margin(1e-9));
    // And a known 3D point: uv (1,2) on the YZ plane = origin + (0,1,0)*1 + (0,0,1)*2.
    const gp_Pnt q = solidops::planePoint3d(Vec2d{1, 2}, plane);
    CHECK(q.X() == Approx(5.0).margin(1e-9));
    CHECK(q.Y() == Approx(-1.0).margin(1e-9));
    CHECK(q.Z() == Approx(9.0).margin(1e-9));
}

TEST_CASE("HOLE ghost preview is a red-effect cylinder at the cursor", "[m7][hole][preview3d]")
{
    Rig rig;
    // Start HOLE with d=6, through — the command now awaits the centre point.
    REQUIRE(rig.processor.submit(QStringLiteral("HOLE 6 T"), false).ok);
    REQUIRE(rig.processor.hasActiveCommand());

    Preview3d ghost;
    REQUIRE(rig.processor.activeCommand()->preview3d(rig.ctx, Vec2d{10, 10}, ghost));
    CHECK(ghost.effect == Preview3d::Effect::Remove); // hole removes material
    REQUIRE_FALSE(ghost.shape.IsNull());
    // Through ghost length = max(depth default 10, 6*d = 36) -> pi*3^2*36.
    CHECK(volumeOf(ghost.shape) == Approx(M_PI * 9.0 * 36.0).epsilon(1e-6));
    // Centred at the cursor on the XY work plane.
    Bnd_Box bb;
    BRepBndLib::Add(ghost.shape, bb);
    double xmin, ymin, zmin, xmax, ymax, zmax;
    bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    CHECK((xmin + xmax) / 2.0 == Approx(10.0).margin(1e-6));
    CHECK((ymin + ymax) / 2.0 == Approx(10.0).margin(1e-6));

    // No ghost before the numeric params are in.
    Rig fresh;
    REQUIRE(fresh.processor.submit(QStringLiteral("HOLE"), false).ok);
    Preview3d none;
    CHECK_FALSE(fresh.processor.activeCommand()->preview3d(fresh.ctx, Vec2d{0, 0}, none));
}

TEST_CASE("shellSolid opens SEVERAL picked faces at once", "[m8][shell][multi]")
{
    using namespace viki;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    // Collect the top and bottom faces (normal ±Z).
    std::vector<TopoDS_Shape> open;
    for (TopExp_Explorer e(box, TopAbs_FACE); e.More(); e.Next()) {
        const auto wp = solidops::planeFromFace(e.Current());
        if (wp && std::abs(wp->normal.Z()) > 0.99)
            open.push_back(e.Current());
    }
    REQUIRE(open.size() == 2);
    const auto tube = solidops::shellSolid(box, 1.0, open);
    REQUIRE(tube.ok);
    // A 10^3 box opened top+bottom with 1mm walls = a square tube:
    // 10*10*10 - 8*8*10 = 360.
    CHECK(volumeOf(tube.shape) == Approx(360.0).epsilon(1e-4));
}

#include <BRepAdaptor_Surface.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <GeomAbs_SurfaceType.hxx>

TEST_CASE("pushPullFace rejects curved faces instead of eating the part",
          "[m7][pushpull][guard]")
{
    using namespace viki;
    const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(10.0, 20.0).Shape();
    int curvedTried = 0;
    for (TopExp_Explorer e(cyl, TopAbs_FACE); e.More(); e.Next()) {
        BRepAdaptor_Surface surf(TopoDS::Face(e.Current()));
        if (surf.GetType() == GeomAbs_Plane)
            continue;
        ++curvedTried;
        const auto r = solidops::pushPullFace(cyl, e.Current(), 5.0);
        // Used to return ok=true with an EMPTY result — the whole part
        // vanished in the GUI. Must now refuse with a clear message.
        CHECK_FALSE(r.ok);
        CHECK(r.message.contains(QStringLiteral("PLANAR")));
    }
    CHECK(curvedTried >= 1);
}

TEST_CASE("booleanOp refuses to return an empty (no-solid) result",
          "[m7][boolean][guard]")
{
    using namespace viki;
    // Subtract a bigger box FROM a smaller one that sits entirely inside it:
    // everything is cut away — that is a reported failure, not a silent empty.
    const TopoDS_Shape small = BRepPrimAPI_MakeBox(2.0, 2.0, 2.0).Shape();
    const TopoDS_Shape big = BRepPrimAPI_MakeBox(
        gp_Pnt(-10, -10, -10), gp_Pnt(10, 10, 10)).Shape();
    const auto gone = solidops::booleanOp(small, big, solidops::BoolOp::Subtract);
    CHECK_FALSE(gone.ok);
    // Intersecting two DISJOINT boxes has no material either.
    const TopoDS_Shape far = BRepPrimAPI_MakeBox(
        gp_Pnt(100, 100, 100), gp_Pnt(110, 110, 110)).Shape();
    const auto none = solidops::booleanOp(small, far, solidops::BoolOp::Intersect);
    CHECK_FALSE(none.ok);
}

TEST_CASE("negative EXTRUDE height goes downward — a hole needs Cut mode",
          "[m7][extrude][semantics]")
{
    // Lex asked: "une extrusion négative génère bien un trou ?" — NO: it
    // extrudes the other way (material below the plane). Cutting material out
    // of an existing solid is EXTRUDE <h> Cut <target> (or HOLE).
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE -10 1")));
    const SolidEntity* down = firstSolid(rig.doc);
    REQUIRE(down);
    CHECK(volumeOf(down->shape()) == Approx(1000.0).epsilon(1e-6));
    CHECK(down->zMin() == Approx(-10.0).margin(1e-6));
    CHECK(down->zMax() == Approx(0.0).margin(1e-6));
}
