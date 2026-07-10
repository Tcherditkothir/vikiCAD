#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QTemporaryDir>

#include "cmd/CommandProcessor.h"
#include "doc/Entities.h"
#include "io/NativeStore.h"
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
    bool run(const QString& line) { return processor.submit(line, true).ok; }
};

int solidCount(const Document& doc)
{
    int n = 0;
    for (const EntityId id : doc.drawOrder())
        if (dynamic_cast<const SolidEntity*>(doc.entity(id)))
            ++n;
    return n;
}

} // namespace

TEST_CASE("sketch registry: create / rename / delete keeps entities", "[sketch]")
{
    Document doc;
    int notifications = 0;
    doc.addChangeListener([&notifications] { ++notifications; });

    const int64_t id = doc.createSketch(QStringLiteral("Base"), WorkPlane{});
    REQUIRE(id != 0);
    REQUIRE(notifications == 1); // registry mutations notify listeners
    REQUIRE(doc.sketchByName(QStringLiteral("base"))); // case-insensitive
    REQUIRE(doc.createSketch(QStringLiteral("Base"), WorkPlane{}) == 0); // dup

    // Entities drawn while the sketch is open get tagged at the addEntity
    // choke point (commands never tag anything themselves).
    doc.setActiveSketch(id);
    doc.beginTransaction(QStringLiteral("draw"));
    const EntityId a =
        doc.addEntity(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{10, 0}));
    doc.commitTransaction();
    doc.setActiveSketch(0);
    doc.beginTransaction(QStringLiteral("draw"));
    const EntityId b =
        doc.addEntity(std::make_unique<LineEntity>(Vec2d{0, 5}, Vec2d{10, 5}));
    doc.commitTransaction();
    REQUIRE(doc.entitySketch(a) == id);
    REQUIRE(doc.entitySketch(b) == 0);
    REQUIRE(doc.sketchEntities(id) == std::vector<EntityId>{a});

    REQUIRE(doc.renameSketch(id, QStringLiteral("Profil")));
    REQUIRE(doc.sketchByName(QStringLiteral("Profil"))->id == id);

    // Delete = registry entry + tags only; the entities stay.
    REQUIRE(doc.removeSketch(id));
    REQUIRE(doc.sketches().empty());
    REQUIRE(doc.entity(a));
    REQUIRE(doc.entitySketch(a) == 0);
}

TEST_CASE("sketch tags survive undo/redo of the tagged entity", "[sketch]")
{
    Document doc;
    const int64_t id = doc.createSketch(QStringLiteral("S"), WorkPlane{});
    doc.setActiveSketch(id);
    doc.beginTransaction(QStringLiteral("draw"));
    const EntityId a =
        doc.addEntity(std::make_unique<CircleEntity>(Vec2d{0, 0}, 5.0));
    doc.commitTransaction();
    doc.setActiveSketch(0);

    doc.undo(); // entity gone; the (now dangling) tag is invisible
    REQUIRE(doc.sketchEntities(id).empty());
    doc.redo(); // entity restored under the SAME id -> tag is back
    REQUIRE(doc.sketchEntities(id) == std::vector<EntityId>{a});
}

TEST_CASE("sketch registry + membership round-trip through .vkd", "[sketch][store]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("sketch.vkd"));

    Document doc;
    // A non-trivial plane (like one set from a picked face).
    const WorkPlane plane{gp_Pnt(10, 20, 30), gp_Dir(0, 1, 0), gp_Dir(0, 0, 1)};
    const int64_t s1 = doc.createSketch(QStringLiteral("Face A"), plane);
    const int64_t s2 = doc.createSketch(QStringLiteral("Face B"), WorkPlane{});
    doc.setActiveSketch(s1);
    doc.beginTransaction(QStringLiteral("draw"));
    const EntityId tagged =
        doc.addEntity(std::make_unique<CircleEntity>(Vec2d{5, 5}, 2.0));
    doc.commitTransaction();
    doc.setActiveSketch(s2); // saved as the open sketch

    QString error;
    REQUIRE(NativeStore::save(doc, path, error));
    const auto loaded = NativeStore::load(path, error);
    REQUIRE(loaded);

    REQUIRE(loaded->sketches().size() == 2);
    const SketchInfo* la = loaded->sketchById(s1);
    REQUIRE(la);
    REQUIRE(la->name == QStringLiteral("Face A"));
    REQUIRE(la->plane.origin.X() == Approx(10.0));
    REQUIRE(la->plane.normal.Y() == Approx(1.0));
    REQUIRE(la->plane.xDir.Z() == Approx(1.0));
    REQUIRE(loaded->entitySketch(tagged) == s1);
    REQUIRE(loaded->activeSketch() == s2);

    // New sketches after load must not collide with stored ids.
    REQUIRE(loaded->createSketch(QStringLiteral("C"), WorkPlane{}) > s2);
}

TEST_CASE("SKETCH New/Close/Open command flow", "[sketch][commands]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("SKETCH NEW poche laterale"))); // spaces ok
    const SketchInfo* info = rig.doc.sketchByName(QStringLiteral("poche laterale"));
    REQUIRE(info);
    REQUIRE(rig.doc.activeSketch() == info->id);

    // Drawn while open -> tagged; the work plane is captured at creation.
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 40,30")));
    const EntityId rect = rig.doc.drawOrder().back();
    REQUIRE(rig.doc.entitySketch(rect) == info->id);

    REQUIRE(rig.run(QStringLiteral("SKETCH CLOSE")));
    REQUIRE(rig.doc.activeSketch() == 0);
    REQUIRE(rig.run(QStringLiteral("CIRCLE 100,0 5")));
    REQUIRE(rig.doc.entitySketch(rig.doc.drawOrder().back()) == 0);

    // Duplicate names are refused (command cancels, registry unchanged).
    rig.run(QStringLiteral("SKETCH NEW poche laterale"));
    REQUIRE(rig.doc.sketches().size() == 1);
    REQUIRE(rig.doc.activeSketch() == 0); // refused -> nothing opened

    // Move the work plane away, then Open restores the sketch's plane and
    // rebuilds reference snap points from its entities.
    REQUIRE(rig.run(QStringLiteral("WORKPLANE OFFSET 25")));
    REQUIRE(documentWorkplane(rig.doc).origin.Z() == Approx(25.0));
    REQUIRE(rig.doc.extraSnapPoints().empty()); // WORKPLANE clears them

    REQUIRE(rig.run(QStringLiteral("SKETCH OPEN poche laterale")));
    REQUIRE(rig.doc.activeSketch() == info->id);
    REQUIRE(documentWorkplane(rig.doc).origin.Z() == Approx(0.0));
    REQUIRE_FALSE(rig.doc.extraSnapPoints().empty()); // rect corners etc.

    // Open by id works too; SKETCH CLOSE drops the reference snaps.
    REQUIRE(rig.run(QStringLiteral("SKETCH CLOSE")));
    REQUIRE(rig.doc.extraSnapPoints().empty());
    REQUIRE(rig.run(QStringLiteral("SKETCH OPEN %1").arg(info->id)));
    REQUIRE(rig.doc.activeSketch() == info->id);

    // Auto-named sketch when no name given.
    REQUIRE(rig.run(QStringLiteral("SKETCH CLOSE")));
    REQUIRE(rig.run(QStringLiteral("SKETCH NEW")));
    REQUIRE(rig.doc.activeSketch() != 0);
    REQUIRE(rig.doc.sketches().size() == 2);
}

TEST_CASE("EXTRUDE keeps sketch profiles, consumes untagged ones",
          "[sketch][commands]")
{
    Rig rig;
    // Untagged profile: consumed (today's behaviour).
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 40,30")));
    const EntityId plain = rig.doc.drawOrder().back();
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 %1").arg(plain)));
    REQUIRE(rig.doc.entityCount() == 1); // only the solid
    REQUIRE(solidCount(rig.doc) == 1);

    // Sketch profile: KEPT (the sketch is a reusable reference).
    REQUIRE(rig.run(QStringLiteral("SKETCH NEW base")));
    REQUIRE(rig.run(QStringLiteral("RECT 100,0 140,30")));
    const EntityId tagged = rig.doc.drawOrder().back();
    REQUIRE(rig.run(QStringLiteral("SKETCH CLOSE")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 %1").arg(tagged)));
    REQUIRE(rig.doc.entity(tagged));     // profile survived
    REQUIRE(solidCount(rig.doc) == 2);   // and the solid was built
    REQUIRE(rig.doc.entityCount() == 3);

    // No dependency solid->sketch: reuse the SAME profile again.
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 20 %1").arg(tagged)));
    REQUIRE(solidCount(rig.doc) == 3);
    REQUIRE(rig.doc.entity(tagged));
}

TEST_CASE("REVOLVE keeps sketch profiles, consumes untagged ones",
          "[sketch][commands]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("SKETCH NEW rev")));
    REQUIRE(rig.run(QStringLiteral("RECT 10,0 20,30")));
    const EntityId tagged = rig.doc.drawOrder().back();
    REQUIRE(rig.run(QStringLiteral("SKETCH CLOSE")));
    REQUIRE(rig.run(QStringLiteral("REVOLVE 360 0,0 0,1 %1").arg(tagged)));
    REQUIRE(rig.doc.entity(tagged)); // kept
    REQUIRE(solidCount(rig.doc) == 1);

    REQUIRE(rig.run(QStringLiteral("RECT 110,0 120,30")));
    const EntityId plain = rig.doc.drawOrder().back();
    REQUIRE(rig.run(QStringLiteral("REVOLVE 360 100,0 100,1 %1").arg(plain)));
    REQUIRE_FALSE(rig.doc.entity(plain)); // consumed
    REQUIRE(solidCount(rig.doc) == 2);
}
