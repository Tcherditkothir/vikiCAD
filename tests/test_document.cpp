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
