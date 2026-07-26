#include <catch2/catch_test_macros.hpp>

#include <QJsonDocument>

#include "doc/Document.h"
#include "doc/Entities.h"

using namespace viki;

namespace {
QByteArray entityJson(const Document& doc, EntityId id)
{
    return QJsonDocument(doc.entity(id)->toJson()).toJson(QJsonDocument::Compact);
}
} // namespace

TEST_CASE("add / remove / undo / redo round-trip", "[document]")
{
    Document doc;
    doc.beginTransaction(QStringLiteral("add"));
    const EntityId id = doc.addEntity(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{10, 0}));
    doc.commitTransaction();
    REQUIRE(doc.entityCount() == 1);
    const QByteArray original = entityJson(doc, id);

    SECTION("undo removes, redo restores byte-identical state")
    {
        doc.undo();
        REQUIRE(doc.entityCount() == 0);
        doc.redo();
        REQUIRE(doc.entityCount() == 1);
        REQUIRE(entityJson(doc, id) == original);
    }

    SECTION("modify is journaled")
    {
        doc.beginTransaction(QStringLiteral("edit"));
        auto* line = static_cast<LineEntity*>(doc.beginModify(id));
        line->transform(Xform2d::translation({5, 5}));
        doc.endModify(id);
        doc.commitTransaction();
        REQUIRE(entityJson(doc, id) != original);
        doc.undo();
        REQUIRE(entityJson(doc, id) == original);
    }

    SECTION("remove then undo restores entity with same id")
    {
        doc.beginTransaction(QStringLiteral("erase"));
        REQUIRE(doc.removeEntity(id));
        doc.commitTransaction();
        REQUIRE(doc.entityCount() == 0);
        doc.undo();
        REQUIRE(doc.entity(id) != nullptr);
        REQUIRE(entityJson(doc, id) == original);
    }

    SECTION("new transaction clears the redo stack")
    {
        doc.undo();
        REQUIRE(doc.canRedo());
        doc.beginTransaction(QStringLiteral("other"));
        doc.addEntity(std::make_unique<CircleEntity>(Vec2d{0, 0}, 5.0));
        doc.commitTransaction();
        REQUIRE_FALSE(doc.canRedo());
    }

    SECTION("rollback undoes uncommitted changes")
    {
        doc.beginTransaction(QStringLiteral("wip"));
        doc.addEntity(std::make_unique<CircleEntity>(Vec2d{1, 1}, 2.0));
        doc.rollbackTransaction();
        REQUIRE(doc.entityCount() == 1);
        REQUIRE_FALSE(doc.canRedo());
    }
}

TEST_CASE("extents", "[document]")
{
    Document doc;
    REQUIRE_FALSE(doc.extents().isValid());
    doc.beginTransaction(QStringLiteral("add"));
    doc.addEntity(std::make_unique<CircleEntity>(Vec2d{10, 10}, 5.0));
    doc.commitTransaction();
    const BBox2d b = doc.extents();
    REQUIRE(b.min.x == 5.0);
    REQUIRE(b.max.y == 15.0);
}

TEST_CASE("stateId identifies undo states, ids never reused", "[document][undo]")
{
    Document doc;
    // As-built state is 0 — the GUI's "same as on disk" baseline for a
    // freshly loaded .vkd (NativeStore builds without transactions).
    REQUIRE(doc.stateId() == 0);

    doc.beginTransaction(QStringLiteral("a"));
    doc.addEntity(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{10, 0}));
    doc.commitTransaction();
    const uint64_t s1 = doc.stateId();
    REQUIRE(s1 != 0);

    doc.beginTransaction(QStringLiteral("b"));
    doc.addEntity(std::make_unique<LineEntity>(Vec2d{0, 5}, Vec2d{10, 5}));
    doc.commitTransaction();
    const uint64_t s2 = doc.stateId();
    REQUIRE(s2 != s1);

    SECTION("undo/redo walk the exact same ids")
    {
        doc.undo();
        CHECK(doc.stateId() == s1);
        doc.redo();
        CHECK(doc.stateId() == s2);
        doc.undo();
        doc.undo();
        CHECK(doc.stateId() == 0); // back to as-built = "not modified"
    }

    SECTION("an abandoned redo branch retires its ids forever")
    {
        doc.undo(); // back to s1, s2 now sits on the redo stack
        doc.beginTransaction(QStringLiteral("c"));
        doc.addEntity(std::make_unique<LineEntity>(Vec2d{5, 0}, Vec2d{5, 10}));
        doc.commitTransaction(); // clears the redo stack
        CHECK(doc.stateId() != s2); // a state saved at s2 stays "modified"
        CHECK(doc.stateId() != s1);
        CHECK(doc.stateId() != 0);
    }

    SECTION("an empty commit does not change the state")
    {
        doc.beginTransaction(QStringLiteral("noop"));
        doc.commitTransaction();
        CHECK(doc.stateId() == s2);
    }
}
