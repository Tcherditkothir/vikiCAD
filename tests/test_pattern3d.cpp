#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include "cmd/CommandProcessor.h"
#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"

using namespace viki;
using Catch::Approx;

namespace {

// A 10x10x10 box whose min corner sits at (x,y,z).
TopoDS_Shape box10(double x, double y, double z)
{
    BRepPrimAPI_MakeBox mk(gp_Pnt(x, y, z), 10.0, 10.0, 10.0);
    return mk.Shape(); // force build (IsDone() unreliable)
}

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    bool run(const QString& s) { return processor.submit(s, true).ok; }
    EntityId addBox(double x, double y, double z)
    {
        doc.beginTransaction(QStringLiteral("box"));
        const EntityId id =
            doc.addEntity(std::make_unique<SolidEntity>(box10(x, y, z)));
        doc.commitTransaction();
        return id;
    }
    int solidCount() const
    {
        int n = 0;
        for (const EntityId id : doc.drawOrder())
            if (dynamic_cast<const SolidEntity*>(doc.entity(id)))
                ++n;
        return n;
    }
};

// Combined bounding box of every solid in the document.
Bnd_Box allSolidsBox(const Document& doc)
{
    Bnd_Box box;
    for (const EntityId id : doc.drawOrder())
        if (const auto* s = dynamic_cast<const SolidEntity*>(doc.entity(id)))
            BRepBndLib::Add(s->shape(), box);
    return box;
}

} // namespace

TEST_CASE("patternRect: cell count and placements", "[pattern3d]")
{
    const auto ts = solidops::patternRect(3, 2, 1, 20, 20, 20);
    REQUIRE(ts.size() == 6u);
    // Cell 0 is the identity (source stays put).
    REQUIRE(ts.front().TranslationPart().X() == Approx(0.0));
    REQUIRE(ts.front().TranslationPart().Y() == Approx(0.0));
    REQUIRE(ts.front().TranslationPart().Z() == Approx(0.0));
    // Counts below 1 clamp to 1.
    REQUIRE(solidops::patternRect(0, -3, 1, 5, 5, 5).size() == 1u);
}

TEST_CASE("PATTERN3D RECT: 3x2x1 box grid spans the expected extent", "[pattern3d]")
{
    Rig rig;
    const EntityId src = rig.addBox(0, 0, 0);
    // Numeric params BEFORE the solid pick: nx ny nz dx dy dz, then the id.
    REQUIRE(rig.run(QStringLiteral("PATTERN3D 3 2 1 20 20 20 %1").arg(src)));

    // 6 solids total (original + 5 clones).
    REQUIRE(rig.solidCount() == 6);

    // Grid of 10mm boxes on a 20mm pitch: X from 0..(2*20+10)=50,
    // Y from 0..(1*20+10)=30, Z from 0..10.
    Bnd_Box b = allSolidsBox(rig.doc);
    double xmin, ymin, zmin, xmax, ymax, zmax;
    b.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    REQUIRE(xmin == Approx(0.0).margin(1e-6));
    REQUIRE(ymin == Approx(0.0).margin(1e-6));
    REQUIRE(zmin == Approx(0.0).margin(1e-6));
    REQUIRE(xmax == Approx(50.0).margin(1e-6));
    REQUIRE(ymax == Approx(30.0).margin(1e-6));
    REQUIRE(zmax == Approx(10.0).margin(1e-6));
}

TEST_CASE("patternPolar: count and full-turn spacing", "[pattern3d]")
{
    // 4 about Z over a full turn: identity + 90/180/270 deg.
    const auto ts =
        solidops::patternPolar(4, gp_Dir(0, 0, 1), gp_Pnt(0, 0, 0), 360.0);
    REQUIRE(ts.size() == 4u);
    // A point at (10,0,0) rotated by the 2nd placement (90 deg) lands ~(0,10,0).
    const gp_Pnt p = gp_Pnt(10, 0, 0).Transformed(ts[1]);
    REQUIRE(p.X() == Approx(0.0).margin(1e-9));
    REQUIRE(p.Y() == Approx(10.0).margin(1e-9));
    REQUIRE(solidops::patternPolar(0, gp_Dir(0, 0, 1), gp_Pnt(0, 0, 0), 90.0)
                .size() == 1u);
}

TEST_CASE("PATTERNPOLAR3D: 4 copies about Z", "[pattern3d]")
{
    Rig rig;
    // Box offset from the Z axis so the ring is non-degenerate.
    const EntityId src = rig.addBox(20, 0, 0);
    // count, axis Z, total angle, center X Y Z, then the id.
    REQUIRE(rig.run(
        QStringLiteral("PATTERNPOLAR3D 4 Z 360 0 0 0 %1").arg(src)));
    REQUIRE(rig.solidCount() == 4);
}
