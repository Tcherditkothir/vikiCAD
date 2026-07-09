#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>

#include "cmd/CommandProcessor.h"
#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"

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

// Sorted volumes of every solid in the document.
std::vector<double> solidVolumes(const Document& doc)
{
    std::vector<double> vols;
    for (const auto* s : allSolids(doc))
        vols.push_back(volumeOf(s->shape()));
    std::sort(vols.begin(), vols.end());
    return vols;
}
} // namespace

TEST_CASE("splitByPlane cuts a 10^3 box at z=4 into 400+600 pieces",
          "[split][splitbody]")
{
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    REQUIRE(volumeOf(box) == Approx(1000.0).epsilon(1e-9));

    const gp_Pln pln(gp_Pnt(0.0, 0.0, 4.0), gp_Dir(0.0, 0.0, 1.0));
    auto pieces = solidops::splitByPlane(box, pln);
    REQUIRE(pieces.size() == 2);

    std::vector<double> vols{volumeOf(pieces[0]), volumeOf(pieces[1])};
    std::sort(vols.begin(), vols.end());
    CHECK(vols[0] == Approx(400.0).epsilon(1e-6));
    CHECK(vols[1] == Approx(600.0).epsilon(1e-6));
    CHECK(vols[0] + vols[1] == Approx(1000.0).epsilon(1e-6));

    // A plane that misses the solid entirely: 0 or 1 piece = no split.
    const gp_Pln miss(gp_Pnt(0.0, 0.0, 50.0), gp_Dir(0.0, 0.0, 1.0));
    CHECK(solidops::splitByPlane(box, miss).size() <= 1);

    // Guards.
    CHECK(solidops::splitByPlane(TopoDS_Shape(), pln).empty());
    CHECK(solidops::splitSolid(box, TopoDS_Shape()).empty());
    CHECK(solidops::splitSolid(TopoDS_Shape(), box).empty());
}

TEST_CASE("splitSolid with a CURVED tool: a cylinder side-face through a box",
          "[split][splitbody]")
{
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();

    // A big cylinder (r=25, axis || Z at (5,-20)) whose LATERAL face sweeps
    // through the box between y ~ 4.49 (at x=0/10) and y = 5 (at x=5).
    const gp_Ax2 axis(gp_Pnt(5.0, -20.0, -10.0), gp_Dir(0.0, 0.0, 1.0));
    const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(axis, 25.0, 30.0).Shape();
    TopoDS_Shape sideFace;
    for (TopExp_Explorer e(cyl, TopAbs_FACE); e.More(); e.Next()) {
        const BRepAdaptor_Surface surf(TopoDS::Face(e.Current()));
        if (surf.GetType() == GeomAbs_Cylinder) {
            sideFace = e.Current();
            break;
        }
    }
    REQUIRE_FALSE(sideFace.IsNull());

    auto pieces = solidops::splitSolid(box, sideFace);
    REQUIRE(pieces.size() == 2);
    const double v0 = volumeOf(pieces[0]);
    const double v1 = volumeOf(pieces[1]);
    CHECK(v0 > 0.0);
    CHECK(v1 > 0.0);
    // The curved cut conserves the material exactly.
    CHECK(v0 + v1 == Approx(1000.0).epsilon(1e-6));
    // The cut runs near y ~ 4.5..5, so the two halves are roughly comparable
    // (neither is a sliver).
    CHECK(std::min(v0, v1) > 300.0);
}

TEST_CASE("SPLIT command replaces the target with the pieces and undoes",
          "[split][splitbody][cmd]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1"))); // solid id 2, vol 1000
    REQUIRE(rig.doc.entityCount() == 1);

    // Tag the target so field inheritance is observable on the pieces.
    rig.doc.beginTransaction(QStringLiteral("tag"));
    if (auto* s = dynamic_cast<SolidEntity*>(rig.doc.beginModify(2))) {
        s->component = QStringLiteral("PartA");
        s->transparency = 0.25;
        rig.doc.endModify(2);
    }
    rig.doc.commitTransaction();

    // SPLIT XY 4 2 : XY plane at z=4, target solid 2 (params before picks).
    REQUIRE(rig.run(QStringLiteral("SPLIT XY 4 2")));
    REQUIRE(rig.doc.entityCount() == 2); // 1 -> 2 pieces
    const auto vols = solidVolumes(rig.doc);
    REQUIRE(vols.size() == 2);
    CHECK(vols[0] == Approx(400.0).epsilon(1e-6));
    CHECK(vols[1] == Approx(600.0).epsilon(1e-6));
    for (const auto* s : allSolids(rig.doc)) {
        CHECK(s->component == QStringLiteral("PartA")); // fields inherited
        CHECK(s->transparency == Approx(0.25));
    }

    // Undo restores the single tagged solid.
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    REQUIRE(rig.doc.entityCount() == 1);
    const auto restored = allSolids(rig.doc);
    REQUIRE(restored.size() == 1);
    CHECK(volumeOf(restored.front()->shape()) == Approx(1000.0).epsilon(1e-6));
}

TEST_CASE("SPLIT Solid mode cuts the target by another solid's boundary",
          "[split][splitbody][cmd]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1"))); // target solid id 2
    REQUIRE(rig.run(QStringLiteral("RECT 4,-1 6,11")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 3"))); // tool solid id 4
    REQUIRE(rig.doc.entityCount() == 2);

    // SPLIT SOLID tool-id target-id: the 2mm-wide tool slab cuts the box at
    // x=4 and x=6 into three slices (400 + 200 + 400); the tool survives.
    REQUIRE(rig.run(QStringLiteral("SPLIT SOLID 4 2")));
    REQUIRE(rig.doc.entityCount() == 4); // tool + 3 pieces
    const auto vols = solidVolumes(rig.doc);
    REQUIRE(vols.size() == 4);
    // Pieces 200, 400, 400 plus the 2*12*10=240 tool.
    CHECK(vols[0] == Approx(200.0).epsilon(1e-6));
    CHECK(vols[1] == Approx(240.0).epsilon(1e-6));
    CHECK(vols[2] == Approx(400.0).epsilon(1e-6));
    CHECK(vols[3] == Approx(400.0).epsilon(1e-6));
}

TEST_CASE("COMBINE fuses two touching boxes into one solid and undoes",
          "[combine][cmd]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1"))); // solid id 2, vol 1000
    REQUIRE(rig.run(QStringLiteral("RECT 10,0 20,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 3"))); // solid id 4, vol 1000
    REQUIRE(rig.doc.entityCount() == 2);

    // Tag the FIRST solid: the combined body inherits its fields.
    rig.doc.beginTransaction(QStringLiteral("tag"));
    if (auto* s = dynamic_cast<SolidEntity*>(rig.doc.beginModify(2))) {
        s->component = QStringLiteral("Alpha");
        rig.doc.endModify(2);
    }
    rig.doc.commitTransaction();

    REQUIRE(rig.run(QStringLiteral("COMBINE 2 4")));
    REQUIRE(rig.doc.entityCount() == 1); // 2 -> 1
    const auto solids = allSolids(rig.doc);
    REQUIRE(solids.size() == 1);
    CHECK(volumeOf(solids.front()->shape()) == Approx(2000.0).epsilon(1e-6));
    CHECK(solids.front()->component == QStringLiteral("Alpha"));

    // Undo restores both boxes.
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    REQUIRE(rig.doc.entityCount() == 2);
    const auto back = solidVolumes(rig.doc);
    CHECK(back[0] == Approx(1000.0).epsilon(1e-6));
    CHECK(back[1] == Approx(1000.0).epsilon(1e-6));

    // FUSE is an alias of COMBINE.
    REQUIRE(rig.run(QStringLiteral("FUSE 2 4")));
    REQUIRE(rig.doc.entityCount() == 1);
    CHECK(volumeOf(allSolids(rig.doc).front()->shape()) ==
          Approx(2000.0).epsilon(1e-6));
}
