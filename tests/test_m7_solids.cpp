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
