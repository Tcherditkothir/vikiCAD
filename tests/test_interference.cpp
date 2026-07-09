#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>
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
    return mk.Shape(); // force build before use (IsDone() is unreliable)
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
};

} // namespace

TEST_CASE("interferenceVolume: overlapping boxes report the slab volume", "[interference]")
{
    // Box A at origin, box B shifted +8 in Z: they overlap by a 10x10x2 slab.
    const TopoDS_Shape a = box10(0, 0, 0);
    const TopoDS_Shape b = box10(0, 0, 8);
    REQUIRE(solidops::interferenceVolume(a, b) == Approx(10.0 * 10.0 * 2.0).epsilon(1e-6));
    // Symmetric.
    REQUIRE(solidops::interferenceVolume(b, a) == Approx(200.0).epsilon(1e-6));
}

TEST_CASE("interferenceVolume: disjoint boxes report zero", "[interference]")
{
    const TopoDS_Shape a = box10(0, 0, 0);
    const TopoDS_Shape b = box10(50, 0, 0); // far away
    REQUIRE(solidops::interferenceVolume(a, b) == Approx(0.0).margin(1e-9));
}

TEST_CASE("interferenceVolume: merely touching boxes report ~zero", "[interference]")
{
    // Face-to-face contact at z=10: common has no solid volume.
    const TopoDS_Shape a = box10(0, 0, 0);
    const TopoDS_Shape b = box10(0, 0, 10);
    REQUIRE(solidops::interferenceVolume(a, b) == Approx(0.0).margin(1e-9));
}

TEST_CASE("interferenceVolume: null shapes are safe", "[interference]")
{
    TopoDS_Shape nul;
    REQUIRE(solidops::interferenceVolume(nul, box10(0, 0, 0)) == 0.0);
    REQUIRE(solidops::interferenceVolume(box10(0, 0, 0), nul) == 0.0);
}

TEST_CASE("checkAllInterferences: finds only the overlapping pair", "[interference]")
{
    Rig rig;
    const EntityId a = rig.addBox(0, 0, 0);
    const EntityId b = rig.addBox(0, 0, 8);  // overlaps a by 200
    rig.addBox(50, 0, 0);                    // disjoint from both

    const auto pairs = solidops::checkAllInterferences(rig.doc);
    REQUIRE(pairs.size() == 1);
    // Reported with a < b ids.
    REQUIRE(pairs[0].a == std::min(a, b));
    REQUIRE(pairs[0].b == std::max(a, b));
    REQUIRE(pairs[0].volume == Approx(200.0).epsilon(1e-6));
}

TEST_CASE("checkAllInterferences: none when all disjoint", "[interference]")
{
    Rig rig;
    rig.addBox(0, 0, 0);
    rig.addBox(50, 0, 0);
    rig.addBox(0, 50, 0);
    REQUIRE(solidops::checkAllInterferences(rig.doc).empty());
}

TEST_CASE("INTERFERE command reports overlap and disjoint", "[interference]")
{
    Rig rig;
    const EntityId a = rig.addBox(0, 0, 0);
    const EntityId b = rig.addBox(0, 0, 8);
    const EntityId c = rig.addBox(50, 0, 0);

    // Overlapping pair.
    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("INTERFERE %1 %2").arg(a).arg(b)));
    bool sawOverlap = false;
    for (const auto& m : rig.ctx.messages())
        if (m.contains(QStringLiteral("interfere")) && m.contains(QStringLiteral("200")))
            sawOverlap = true;
    REQUIRE(sawOverlap);

    // Disjoint pair.
    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("INTERFERE %1 %2").arg(a).arg(c)));
    bool sawDisjoint = false;
    for (const auto& m : rig.ctx.messages())
        if (m.contains(QStringLiteral("do not interfere")))
            sawDisjoint = true;
    REQUIRE(sawDisjoint);
}
