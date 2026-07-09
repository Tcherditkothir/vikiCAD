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
    Rig()
    {
        registerBuiltinCommands(processor);
        // documentWorkplane is keyed by Document address: a recycled address
        // could inherit a stale plane from a previous test. Start from XY.
        documentWorkplane(doc) = WorkPlane{};
    }
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

// Bounding box of one solid entity.
Bnd_Box solidBox(const Document& doc, EntityId id)
{
    Bnd_Box box;
    if (const auto* s = dynamic_cast<const SolidEntity*>(doc.entity(id)))
        BRepBndLib::Add(s->shape(), box);
    return box;
}

void expectBoxMin(const Bnd_Box& b, double x, double y, double z)
{
    double xmin, ymin, zmin, xmax, ymax, zmax;
    b.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    REQUIRE(xmin == Approx(x).margin(1e-6));
    REQUIRE(ymin == Approx(y).margin(1e-6));
    REQUIRE(zmin == Approx(z).margin(1e-6));
    REQUIRE(xmax == Approx(x + 10.0).margin(1e-6));
    REQUIRE(ymax == Approx(y + 10.0).margin(1e-6));
    REQUIRE(zmax == Approx(z + 10.0).margin(1e-6));
}

} // namespace

TEST_CASE("MOVE3D P: two points on the XY work plane translate in X/Y",
          "[move3d]")
{
    Rig rig;
    const EntityId id = rig.addBox(0, 0, 0);
    // Keyword P at the dx prompt switches to point mode: base 0,0 dest 10,5.
    REQUIRE(rig.run(QStringLiteral("MOVE3D P 0,0 10,5 %1").arg(id)));
    expectBoxMin(solidBox(rig.doc, id), 10.0, 5.0, 0.0);
}

TEST_CASE("MOVE3D P: the same picks on a YZ work plane move in Y/Z",
          "[move3d]")
{
    Rig rig;
    const EntityId id = rig.addBox(0, 0, 0);
    // YZ plane: normal = +X, sketch u axis = world Y, so v = X x Y = Z.
    documentWorkplane(rig.doc) =
        WorkPlane{gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0), gp_Dir(0, 1, 0)};
    REQUIRE(rig.run(QStringLiteral("MOVE3D P 0,0 10,5 %1").arg(id)));
    expectBoxMin(solidBox(rig.doc, id), 0.0, 10.0, 5.0);
}

TEST_CASE("MOVE3D numeric dx dy dz path is unchanged", "[move3d]")
{
    Rig rig;
    const EntityId id = rig.addBox(0, 0, 0);
    REQUIRE(rig.run(QStringLiteral("MOVE3D 5 -3 2 %1").arg(id)));
    expectBoxMin(solidBox(rig.doc, id), 5.0, -3.0, 2.0);

    // Undo restores the original placement (one transaction).
    REQUIRE(rig.run(QStringLiteral("UNDO")));
    expectBoxMin(solidBox(rig.doc, id), 0.0, 0.0, 0.0);
}
