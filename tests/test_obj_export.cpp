#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <set>

#include "cmd/CommandProcessor.h"
#include "doc/Document.h"
#include "doc/SelectionSet.h"
#include "io/ObjIo.h"

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

// A 10x10x10 box: RECT then EXTRUDE 10.
void buildBox(Rig& rig)
{
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 1")));
}

struct ParsedObj {
    int vertexLines = 0;
    int faceLines = 0;
    int triangleFaces = 0;
    std::set<std::tuple<long long, long long, long long>> uniqueVertices;
};

// Minimal OBJ parser: counts v/f lines, tracks unique vertex positions (quantized
// so tessellation float noise coalesces), and checks that every face is a
// triangle. Face vertex tokens may be "i", "i//n" or "i/t/n".
ParsedObj parseObj(const QByteArray& bytes)
{
    ParsedObj out;
    const QString text = QString::fromUtf8(bytes);
    const QStringList lines = text.split('\n');
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("v "))) {
            const QStringList tok =
                line.split(' ', Qt::SkipEmptyParts);
            REQUIRE(tok.size() >= 4);
            const auto q = [](const QString& s) {
                return static_cast<long long>(s.toDouble() * 1000.0 + 0.5);
            };
            out.uniqueVertices.insert({q(tok[1]), q(tok[2]), q(tok[3])});
            ++out.vertexLines;
        } else if (line.startsWith(QLatin1String("f "))) {
            const QStringList tok = line.split(' ', Qt::SkipEmptyParts);
            const int nverts = tok.size() - 1;
            if (nverts == 3)
                ++out.triangleFaces;
            ++out.faceLines;
        }
    }
    return out;
}
} // namespace

TEST_CASE("exportObj meshes a 10mm box into a parseable OBJ", "[obj]")
{
    Rig rig;
    buildBox(rig);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("box.obj"));

    const ObjResult r = exportObj(rig.doc, path, 0.1);
    REQUIRE(r.ok);
    CHECK(r.solids == 1);

    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    f.close();

    // Non-empty and starts with OBJ content (a comment header here).
    CHECK(bytes.size() > 0);
    CHECK(bytes.startsWith("# Wavefront OBJ"));

    const ParsedObj parsed = parseObj(bytes);

    // A closed box tessellates to exactly 12 triangles (2 per planar face * 6
    // faces). Every emitted face must be a triangle.
    CHECK(parsed.faceLines == 12);
    CHECK(parsed.triangleFaces == 12);

    // The 8 geometric corners of the cube collapse to >= 8 unique positions.
    // (OCCT emits per-face vertex blocks, so raw v lines are >= 8, but the
    // distinct coordinates are exactly the 8 box corners.)
    CHECK(parsed.uniqueVertices.size() >= 8);
    CHECK(parsed.uniqueVertices.size() == 8);
    CHECK(parsed.vertexLines >= 8);
}

TEST_CASE("exportObj fails cleanly when there are no solids", "[obj]")
{
    Document doc;
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("empty.obj"));

    const ObjResult r = exportObj(doc, path, 0.1);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.isEmpty());
}
