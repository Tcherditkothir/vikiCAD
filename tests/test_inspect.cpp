#include <catch2/catch_test_macros.hpp>

#include "cmd/CommandProcessor.h"
#include "solid/SolidEntity.h"
#include "solid/SubShape.h"

using namespace viki;

namespace {

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    bool run(const QString& s) { return processor.submit(s, true).ok; }
};

const SolidEntity* firstSolid(const Document& doc)
{
    for (const EntityId id : doc.drawOrder())
        if (const auto* s = dynamic_cast<const SolidEntity*>(doc.entity(id)))
            return s;
    return nullptr;
}

EntityId firstSolidId(const Document& doc)
{
    for (const EntityId id : doc.drawOrder())
        if (dynamic_cast<const SolidEntity*>(doc.entity(id)))
            return id;
    return kInvalidEntityId;
}

int countStarting(const std::vector<QString>& msgs, const QString& prefix)
{
    int n = 0;
    for (const QString& m : msgs)
        n += m.startsWith(prefix) ? 1 : 0;
    return n;
}

bool anyContains(const std::vector<QString>& msgs, const QString& needle)
{
    for (const QString& m : msgs)
        if (m.contains(needle))
            return true;
    return false;
}

} // namespace

TEST_CASE("INSPECT a box lists 6 planar faces and 12 line edges", "[inspect]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 40,30")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 20 1")));
    const EntityId id = firstSolidId(rig.doc);
    REQUIRE(id != kInvalidEntityId);

    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("INSPECT %1").arg(id))); // default = All
    const auto& msgs = rig.ctx.messages();

    REQUIRE(anyContains(msgs, QStringLiteral("6 faces, 12 edges")));
    REQUIRE(countStarting(msgs, QStringLiteral("face ")) == 6);
    REQUIRE(countStarting(msgs, QStringLiteral("edge ")) == 12);
    // A box is all planes and lines; every line carries area/len + position.
    for (const QString& m : msgs) {
        if (m.startsWith(QStringLiteral("face "))) {
            REQUIRE(m.contains(QStringLiteral("plane")));
            REQUIRE(m.contains(QStringLiteral("area=")));
            REQUIRE(m.contains(QStringLiteral("centroid=(")));
        }
        if (m.startsWith(QStringLiteral("edge "))) {
            REQUIRE(m.contains(QStringLiteral("line")));
            REQUIRE(m.contains(QStringLiteral("len=")));
            REQUIRE(m.contains(QStringLiteral("mid=(")));
        }
    }
    // The exact line format is the agents' contract: "face 0: plane area=..."
    REQUIRE(anyContains(msgs, QStringLiteral("face 0: plane area=")));
    REQUIRE(anyContains(msgs, QStringLiteral("edge 11: line len=")));

    // INS alias + Faces / Edges scopes.
    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("INS %1 F").arg(id)));
    REQUIRE(countStarting(rig.ctx.messages(), QStringLiteral("face ")) == 6);
    REQUIRE(countStarting(rig.ctx.messages(), QStringLiteral("edge ")) == 0);

    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("INSPECT %1 Edges").arg(id)));
    REQUIRE(countStarting(rig.ctx.messages(), QStringLiteral("face ")) == 0);
    REQUIRE(countStarting(rig.ctx.messages(), QStringLiteral("edge ")) == 12);
}

TEST_CASE("INSPECT reports cylinder faces and circle edges", "[inspect]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("CIRCLE 0,0 10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 20 1")));
    const EntityId id = firstSolidId(rig.doc);
    REQUIRE(id != kInvalidEntityId);

    rig.ctx.clearMessages();
    REQUIRE(rig.run(QStringLiteral("INSPECT %1 All").arg(id)));
    const auto& msgs = rig.ctx.messages();
    REQUIRE(anyContains(msgs, QStringLiteral("cylinder r=10.0")));
    REQUIRE(anyContains(msgs, QStringLiteral("circle r=10.0")));
}

TEST_CASE("INSPECT refuses a non-solid id", "[inspect]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("CIRCLE 0,0 10")));
    rig.ctx.clearMessages();
    rig.run(QStringLiteral("INSPECT 1")); // cancels, but must not crash
    REQUIRE(anyContains(rig.ctx.messages(),
                        QStringLiteral("that id is not a solid")));
}

TEST_CASE("faceAt/edgeAt round-trip with the printed indices", "[inspect]")
{
    Rig rig;
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 40,30")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 20 1")));
    const SolidEntity* solid = firstSolid(rig.doc);
    REQUIRE(solid);
    const TopoDS_Shape& shape = solid->shape();

    REQUIRE(subshape::faceCount(shape) == 6);
    REQUIRE(subshape::edgeCount(shape) == 12);

    for (int i = 0; i < 6; ++i) {
        const TopoDS_Shape face = subshape::faceAt(shape, i);
        REQUIRE_FALSE(face.IsNull());
        REQUIRE(subshape::faceIndexOf(shape, face) == i);
    }
    for (int i = 0; i < 12; ++i) {
        const TopoDS_Shape edge = subshape::edgeAt(shape, i);
        REQUIRE_FALSE(edge.IsNull());
        REQUIRE(subshape::edgeIndexOf(shape, edge) == i);
    }

    // Out-of-range and cross-kind queries answer null / -1, never throw.
    REQUIRE(subshape::faceAt(shape, 6).IsNull());
    REQUIRE(subshape::faceAt(shape, -1).IsNull());
    REQUIRE(subshape::edgeAt(shape, 12).IsNull());
    REQUIRE(subshape::faceIndexOf(shape, subshape::edgeAt(shape, 0)) == -1);
    REQUIRE(subshape::edgeIndexOf(shape, subshape::faceAt(shape, 0)) == -1);
    REQUIRE(subshape::faceIndexOf(shape, TopoDS_Shape()) == -1);
    REQUIRE(subshape::faceCount(TopoDS_Shape()) == 0);
    REQUIRE(subshape::edgeAt(TopoDS_Shape(), 0).IsNull());
}
