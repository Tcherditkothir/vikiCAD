#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include "cmd/CommandProcessor.h"
#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"

using namespace viki;
using Catch::Approx;

namespace {

// A 10x10x10 box with its min corner at the origin.
TopoDS_Shape box10()
{
    BRepPrimAPI_MakeBox mk(gp_Pnt(0, 0, 0), 10.0, 10.0, 10.0);
    return mk.Shape(); // force build before use (IsDone() is unreliable)
}

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    bool run(const QString& s) { return processor.submit(s, true).ok; }
    EntityId addBox()
    {
        doc.beginTransaction(QStringLiteral("box"));
        const EntityId id = doc.addEntity(std::make_unique<SolidEntity>(box10()));
        doc.commitTransaction();
        return id;
    }
};

} // namespace

TEST_CASE("sectionArea: z=5 plane through a 10^3 box gives area ~100", "[section]")
{
    const TopoDS_Shape b = box10();
    const gp_Pln z5(gp_Pnt(0, 0, 5), gp_Dir(0, 0, 1));
    REQUIRE(solidops::sectionArea(b, z5) == Approx(100.0).epsilon(1e-6));
}

TEST_CASE("sectionArea: off-center YZ plane still cuts the full 10x10 face", "[section]")
{
    const TopoDS_Shape b = box10();
    // Plane x=3, normal along X: the section is the 10x10 y-z face.
    const gp_Pln x3(gp_Pnt(3, 0, 0), gp_Dir(1, 0, 0));
    REQUIRE(solidops::sectionArea(b, x3) == Approx(100.0).epsilon(1e-6));
}

TEST_CASE("sectionArea: a plane that misses the solid returns 0", "[section]")
{
    const TopoDS_Shape b = box10();
    const gp_Pln z50(gp_Pnt(0, 0, 50), gp_Dir(0, 0, 1)); // well above the box
    REQUIRE(solidops::sectionArea(b, z50) == Approx(0.0).margin(1e-9));
}

TEST_CASE("sectionArea: null shape is safe", "[section]")
{
    TopoDS_Shape nul;
    const gp_Pln z5(gp_Pnt(0, 0, 5), gp_Dir(0, 0, 1));
    REQUIRE(solidops::sectionArea(nul, z5) == 0.0);
}

TEST_CASE("sectionWires: the z=5 section is one closed wire", "[section]")
{
    const TopoDS_Shape b = box10();
    const gp_Pln z5(gp_Pnt(0, 0, 5), gp_Dir(0, 0, 1));
    const TopoDS_Shape wires = solidops::sectionWires(b, z5);
    REQUIRE_FALSE(wires.IsNull());
    int nWires = 0;
    bool allClosed = true;
    for (TopExp_Explorer ex(wires, TopAbs_WIRE); ex.More(); ex.Next()) {
        ++nWires;
        if (!ex.Current().Closed())
            allClosed = false;
    }
    REQUIRE(nWires == 1);
    REQUIRE(allClosed);
}

TEST_CASE("SECTION command reports the section area headless", "[section]")
{
    Rig rig;
    const EntityId id = rig.addBox();
    rig.ctx.clearMessages();
    // Plane keyword XY, offset 5, then the solid id.
    REQUIRE(rig.run(QStringLiteral("SECTION XY 5 %1").arg(id)));
    bool sawArea = false;
    for (const auto& m : rig.ctx.messages())
        if (m.contains(QStringLiteral("section area")) &&
            m.contains(QStringLiteral("100")))
            sawArea = true;
    REQUIRE(sawArea);
}

TEST_CASE("SECTION command: plane missing the solid is reported", "[section]")
{
    Rig rig;
    const EntityId id = rig.addBox();
    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("SECTION XY 50 %1").arg(id)));
    bool sawMiss = false;
    for (const auto& m : rig.ctx.messages())
        if (m.contains(QStringLiteral("does not cut")))
            sawMiss = true;
    REQUIRE(sawMiss);
}
