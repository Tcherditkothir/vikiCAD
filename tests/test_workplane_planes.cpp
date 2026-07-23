#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>

#include "cmd/CommandProcessor.h"
#include "doc/Entities.h"
#include "script/ScriptRunner.h"
#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"

// WORKPLANE XY|XZ|YZ [OFFSET d] — the world-aligned sketch planes that start
// a blank 3D project. Every expected value below is DERIVED BY HAND from the
// frame table in Commands3D.cpp (and cross-checked against StandardViews):
//   XY: u=+X v=+Y n=+Z   (a,b) at OFFSET d -> world (a,  b, d)  TOP-upright
//   XZ: u=+X v=+Z n=-Y   (a,b) at OFFSET d -> world (a, -d, b)  FRONT-upright
//   YZ: u=+Y v=+Z n=+X   (a,b) at OFFSET d -> world (d,  a, b)  RIGHT-upright
// planePoint3d maps (u,v) -> origin + u*xDir + v*(normal x xDir), so these
// checks pin the whole frame (origin, xDir AND the derived v axis) at once.

using namespace viki;
using Catch::Approx;

namespace {

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    // The per-document work-plane registry is keyed by Document ADDRESS: a
    // fresh Rig can inherit a stale plane from a destroyed sibling document
    // at the same address. Every rig starts by pinning the default.
    ~Rig() { processor.submit(QStringLiteral("WORKPLANE XY"), true); }
    bool run(const QString& line) { return processor.submit(line, true).ok; }
};

const SolidEntity* firstSolid(const Document& doc)
{
    for (const EntityId id : doc.drawOrder())
        if (const auto* s = dynamic_cast<const SolidEntity*>(doc.entity(id)))
            return s;
    return nullptr;
}

void checkPoint(const gp_Pnt& p, double x, double y, double z)
{
    CHECK(p.X() == Approx(x).margin(1e-9));
    CHECK(p.Y() == Approx(y).margin(1e-9));
    CHECK(p.Z() == Approx(z).margin(1e-9));
}

void checkDir(const gp_Dir& d, double x, double y, double z)
{
    CHECK(d.X() == Approx(x).margin(1e-12));
    CHECK(d.Y() == Approx(y).margin(1e-12));
    CHECK(d.Z() == Approx(z).margin(1e-12));
}

} // namespace

TEST_CASE("WORKPLANE back-compat: bare and legacy OFFSET forms unchanged",
          "[workplane]")
{
    Rig rig;

    // Bare WORKPLANE (implicit Finish in strict mode) -> world XY at Z=0.
    REQUIRE(rig.run(QStringLiteral("WORKPLANE")));
    {
        const WorkPlane& p = documentWorkplane(rig.doc);
        checkPoint(p.origin, 0, 0, 0);
        checkDir(p.normal, 0, 0, 1);
        checkDir(p.xDir, 1, 0, 0);
    }

    // Legacy grammar: OFFSET implies the XY plane. Z=100 exactly as before.
    REQUIRE(rig.run(QStringLiteral("WORKPLANE OFFSET 100")));
    {
        const WorkPlane& p = documentWorkplane(rig.doc);
        checkPoint(p.origin, 0, 0, 100);
        checkDir(p.normal, 0, 0, 1);
        checkDir(p.xDir, 1, 0, 0);
        // Sketch (a,b)=(3,7) -> world (3, 7, 100).
        checkPoint(solidops::planePoint3d(Vec2d{3, 7}, p), 3, 7, 100);
    }

    // Explicit XY keyword (new grammar) is the same plane.
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY")));
    {
        const WorkPlane& p = documentWorkplane(rig.doc);
        checkPoint(p.origin, 0, 0, 0);
        checkDir(p.normal, 0, 0, 1);
        checkDir(p.xDir, 1, 0, 0);
    }

    // And XY takes the new uniform OFFSET too: same as the legacy form.
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XY OFFSET 4")));
    checkPoint(solidops::planePoint3d(Vec2d{3, 7}, documentWorkplane(rig.doc)),
               3, 7, 4);
}

TEST_CASE("WORKPLANE XZ frame: u=+X v=+Z n=-Y, offset along -Y", "[workplane]")
{
    Rig rig;

    REQUIRE(rig.run(QStringLiteral("WORKPLANE XZ")));
    {
        const WorkPlane& p = documentWorkplane(rig.doc);
        checkPoint(p.origin, 0, 0, 0);
        checkDir(p.normal, 0, -1, 0); // faces the FRONT camera (look dir +Y)
        checkDir(p.xDir, 1, 0, 0);
        // Sketch (a,b)=(7,3) -> world (7, 0, 3): u carries X, v carries Z.
        checkPoint(solidops::planePoint3d(Vec2d{7, 3}, p), 7, 0, 3);
    }

    // OFFSET 5 moves the plane 5 mm ALONG THE NORMAL (-Y): (a,b) -> (a,-5,b).
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XZ OFFSET 5")));
    {
        const WorkPlane& p = documentWorkplane(rig.doc);
        checkPoint(p.origin, 0, -5, 0);
        checkPoint(solidops::planePoint3d(Vec2d{7, 3}, p), 7, -5, 3);
    }
}

TEST_CASE("WORKPLANE YZ frame: u=+Y v=+Z n=+X, offset along +X", "[workplane]")
{
    Rig rig;

    REQUIRE(rig.run(QStringLiteral("WORKPLANE YZ")));
    {
        const WorkPlane& p = documentWorkplane(rig.doc);
        checkPoint(p.origin, 0, 0, 0);
        checkDir(p.normal, 1, 0, 0); // faces the RIGHT camera (look dir -X)
        checkDir(p.xDir, 0, 1, 0);
        // Sketch (a,b)=(7,3) -> world (0, 7, 3): u carries Y, v carries Z.
        checkPoint(solidops::planePoint3d(Vec2d{7, 3}, p), 0, 7, 3);
    }

    REQUIRE(rig.run(QStringLiteral("WORKPLANE YZ OFFSET 4")));
    {
        const WorkPlane& p = documentWorkplane(rig.doc);
        checkPoint(p.origin, 4, 0, 0);
        checkPoint(solidops::planePoint3d(Vec2d{7, 3}, p), 4, 7, 3);
    }
}

TEST_CASE("SKETCH New captures each world plane; drawn points land where the "
          "matching view expects",
          "[workplane][sketch]")
{
    struct Case {
        const char* wp;      // WORKPLANE command
        const char* name;    // mono-token sketch name
        double wx, wy, wz;   // expected world position of sketch point (7,3)
    };
    // Hand-derived from the frame table (offset 2 on each plane):
    //   XY: (7, 3, 2)   XZ: (7, -2, 3)   YZ: (2, 7, 3)
    const Case cases[] = {
        {"WORKPLANE XY OFFSET 2", "on-xy", 7, 3, 2},
        {"WORKPLANE XZ OFFSET 2", "on-xz", 7, -2, 3},
        {"WORKPLANE YZ OFFSET 2", "on-yz", 2, 7, 3},
    };
    Rig rig;
    for (const Case& c : cases) {
        REQUIRE(rig.run(QLatin1String(c.wp)));
        REQUIRE(rig.run(QStringLiteral("SKETCH NEW %1").arg(QLatin1String(c.name))));
        REQUIRE(rig.run(QStringLiteral("LINE 7,3 8,4")));
        REQUIRE(rig.run(QStringLiteral("SKETCH CLOSE")));
        const SketchInfo* info = rig.doc.sketchByName(QLatin1String(c.name));
        REQUIRE(info);
        // The sketch stored the ACTIVE plane (this is the plane the 3D view
        // uses to place the sketch's curves — OcctViewWidget::refreshFrom).
        checkPoint(solidops::planePoint3d(Vec2d{7, 3}, info->plane),
                   c.wx, c.wy, c.wz);
    }
}

TEST_CASE("EXTRUDE of an XZ profile builds the solid where FRONT expects",
          "[workplane][extrude]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("WORKPLANE XZ")));
    REQUIRE(rig.run(QStringLiteral("SKETCH NEW front-profile")));
    REQUIRE(rig.run(QStringLiteral("RECT 10,20 30,50"))); // entity id 1
    REQUIRE(rig.run(QStringLiteral("SKETCH CLOSE")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 8 1")));

    const SolidEntity* solid = firstSolid(rig.doc);
    REQUIRE(solid);
    // By hand: u=10..30 -> X 10..30; v=20..50 -> Z 20..50; extrusion runs
    // along the normal -Y -> Y -8..0. Volume 20*30*8 = 4800.
    GProp_GProps props;
    BRepGProp::VolumeProperties(solid->shape(), props);
    CHECK(props.Mass() == Approx(4800.0).epsilon(1e-6));
    Bnd_Box bb;
    BRepBndLib::Add(solid->shape(), bb);
    double xmn, ymn, zmn, xmx, ymx, zmx;
    bb.Get(xmn, ymn, zmn, xmx, ymx, zmx);
    CHECK(xmn == Approx(10.0).margin(1e-6));
    CHECK(xmx == Approx(30.0).margin(1e-6));
    CHECK(ymn == Approx(-8.0).margin(1e-6));
    CHECK(ymx == Approx(0.0).margin(1e-6));
    CHECK(zmn == Approx(20.0).margin(1e-6));
    CHECK(zmx == Approx(50.0).margin(1e-6));

    // The tagged profile survived the EXTRUDE (sketch semantics unchanged).
    CHECK(rig.doc.entity(1) != nullptr);
}

TEST_CASE("WORKPLANE's optional stages hand unknown tokens back to the "
          "processor instead of swallowing the next command",
          "[workplane][script]")
{
    // The verifier's .vks trap: 'WORKPLANE XZ' left its optional-OFFSET
    // stage pending, the next line's first token (RECT) terminated it as an
    // unrecognized keyword and finishCommand dropped the REST of that line —
    // an EMPTY document with ok:true. The fix is Step repush: the finished
    // command declares the token unconsumed and the processor re-dispatches
    // it (plus everything after it) as a fresh command line, the AutoCAD
    // .scr behavior.

    SECTION(".vks script: WORKPLANE XZ directly followed by RECT + EXTRUDE")
    {
        Rig rig;
        const auto r = runScript(
            rig.processor,
            QStringLiteral("WORKPLANE XZ\nRECT 0,0 10,10\nEXTRUDE 3 1\n"));
        REQUIRE(r.ok);
        checkDir(documentWorkplane(rig.doc).normal, 0, -1, 0); // XZ stuck
        const SolidEntity* solid = firstSolid(rig.doc);
        REQUIRE(solid); // the trap produced ZERO entities here
        // By hand on XZ: u=0..10 -> X, v=0..10 -> Z, EXTRUDE 3 along -Y.
        GProp_GProps props;
        BRepGProp::VolumeProperties(solid->shape(), props);
        CHECK(props.Mass() == Approx(300.0).epsilon(1e-6));
        Bnd_Box bb;
        BRepBndLib::Add(solid->shape(), bb);
        double xmn, ymn, zmn, xmx, ymx, zmx;
        bb.Get(xmn, ymn, zmn, xmx, ymx, zmx);
        CHECK(xmn == Approx(0.0).margin(1e-6));
        CHECK(xmx == Approx(10.0).margin(1e-6));
        CHECK(ymn == Approx(-3.0).margin(1e-6));
        CHECK(ymx == Approx(0.0).margin(1e-6));
        CHECK(zmn == Approx(0.0).margin(1e-6));
        CHECK(zmx == Approx(10.0).margin(1e-6));
    }

    SECTION(".vks script: bare WORKPLANE directly followed by CIRCLE")
    {
        // Same trap one stage earlier (the plane-keyword prompt itself).
        Rig rig;
        const auto r = runScript(rig.processor,
                                 QStringLiteral("WORKPLANE\nCIRCLE 5,5 2\n"));
        REQUIRE(r.ok);
        checkDir(documentWorkplane(rig.doc).normal, 0, 0, 1); // XY default
        CHECK(rig.doc.drawOrder().size() == 1);
    }

    SECTION("command bar: RECT typed while WORKPLANE XZ waits for its offset")
    {
        Rig rig;
        const auto r1 = rig.processor.submit(QStringLiteral("WORKPLANE XZ"));
        REQUIRE(r1.ok);
        REQUIRE(r1.pending); // waiting at 'Offset [OFFSET] <0>:'
        const auto r2 =
            rig.processor.submit(QStringLiteral("RECT 0,0 10,10"));
        REQUIRE(r2.ok);
        CHECK_FALSE(rig.processor.hasActiveCommand());
        CHECK(rig.doc.drawOrder().size() == 1);
        checkDir(documentWorkplane(rig.doc).normal, 0, -1, 0);
    }

    SECTION("one strict line chains: WORKPLANE XZ RECT 0,0 10,10")
    {
        Rig rig;
        REQUIRE(rig.run(QStringLiteral("WORKPLANE XZ RECT 0,0 10,10")));
        CHECK(rig.doc.drawOrder().size() == 1);
        checkDir(documentWorkplane(rig.doc).normal, 0, -1, 0);
    }
}
