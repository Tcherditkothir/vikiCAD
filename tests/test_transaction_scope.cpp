#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include "cmd/CommandProcessor.h"
#include "doc/Document.h"
#include "doc/Entities.h"

using namespace viki;

namespace {

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
};

} // namespace

TEST_CASE("TransactionScope commits like a plain transaction", "[document]")
{
    Document doc;
    {
        TransactionScope scope(doc, QStringLiteral("add"));
        doc.addEntity(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{10, 0}));
        scope.commit();
    }
    REQUIRE(doc.entityCount() == 1);
    REQUIRE_FALSE(doc.inTransaction());
    REQUIRE(doc.canUndo());
    doc.undo();
    REQUIRE(doc.entityCount() == 0);
}

TEST_CASE("TransactionScope rolls back on early exit without commit", "[document]")
{
    Document doc;
    {
        TransactionScope scope(doc, QStringLiteral("wip"));
        doc.addEntity(std::make_unique<CircleEntity>(Vec2d{1, 1}, 2.0));
        // simulate an early return: scope destroyed without commit()
    }
    REQUIRE(doc.entityCount() == 0);
    REQUIRE_FALSE(doc.inTransaction());
    REQUIRE_FALSE(doc.canUndo());
}

TEST_CASE("TransactionScope rolls back when an exception escapes", "[document]")
{
    Document doc;
    const auto boom = [&doc] {
        TransactionScope scope(doc, QStringLiteral("boom"));
        doc.addEntity(std::make_unique<CircleEntity>(Vec2d{0, 0}, 5.0));
        throw std::runtime_error("OCCT blew up mid-transaction");
        // never reached: scope.commit();
    };
    REQUIRE_THROWS_AS(boom(), std::runtime_error);
    // The document must be exactly as before: no entity, no open transaction
    // (an open one would silently kill undo forever), nothing on the stacks.
    REQUIRE(doc.entityCount() == 0);
    REQUIRE_FALSE(doc.inTransaction());
    REQUIRE_FALSE(doc.canUndo());

    // And undo still works for real transactions afterwards.
    {
        TransactionScope scope(doc, QStringLiteral("after"));
        doc.addEntity(std::make_unique<LineEntity>(Vec2d{0, 0}, Vec2d{1, 0}));
        scope.commit();
    }
    REQUIRE(doc.canUndo());
}

TEST_CASE("explicit rollback closes the scope once", "[document]")
{
    Document doc;
    {
        TransactionScope scope(doc, QStringLiteral("rejected"));
        doc.addEntity(std::make_unique<CircleEntity>(Vec2d{3, 3}, 1.0));
        scope.rollback(); // the dtor must NOT roll back a second time
        REQUIRE_FALSE(doc.inTransaction());
    }
    REQUIRE(doc.entityCount() == 0);
    REQUIRE_FALSE(doc.inTransaction());
}

TEST_CASE("UNDO recovers from a leaked open transaction", "[commands]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("LINE 0,0 10,0"), true).ok);
    REQUIRE(rig.doc.entityCount() == 1);

    // Simulate the field bug: some code path opened a transaction and never
    // closed it. Document::undo() alone would refuse forever.
    rig.doc.beginTransaction(QStringLiteral("LEAKED"));
    REQUIRE(rig.doc.inTransaction());

    // The UNDO command rolls the stray transaction back, reports, and the
    // undo itself goes through.
    REQUIRE(rig.processor.submit(QStringLiteral("UNDO"), true).ok);
    REQUIRE_FALSE(rig.doc.inTransaction());
    REQUIRE(rig.doc.entityCount() == 0);
}

TEST_CASE("REDO recovers from a leaked open transaction", "[commands]")
{
    Rig rig;
    REQUIRE(rig.processor.submit(QStringLiteral("LINE 0,0 10,0"), true).ok);
    REQUIRE(rig.processor.submit(QStringLiteral("UNDO"), true).ok);
    REQUIRE(rig.doc.entityCount() == 0);

    rig.doc.beginTransaction(QStringLiteral("LEAKED"));
    REQUIRE(rig.doc.inTransaction());

    REQUIRE(rig.processor.submit(QStringLiteral("REDO"), true).ok);
    REQUIRE_FALSE(rig.doc.inTransaction());
    REQUIRE(rig.doc.entityCount() == 1);
}
