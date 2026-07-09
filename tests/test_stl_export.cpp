#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

#include <cstdint>
#include <cstring>

#include "cmd/CommandProcessor.h"
#include "doc/Document.h"
#include "doc/SelectionSet.h"
#include "io/StlIo.h"

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

// Read the 4-byte little-endian triangle count from a binary STL: an 80-byte
// header is followed by a uint32 facet count.
std::uint32_t binaryStlTriangleCount(const QByteArray& bytes)
{
    REQUIRE(bytes.size() >= 84);
    std::uint32_t n = 0;
    std::memcpy(&n, bytes.constData() + 80, sizeof(n));
    return n;
}
} // namespace

TEST_CASE("exportStl meshes a 10mm box into a non-empty binary STL", "[stl]")
{
    Rig rig;
    buildBox(rig);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("box.stl"));

    const StlResult r = exportStl(rig.doc, path, 0.1, /*ascii=*/false);
    REQUIRE(r.ok);
    CHECK(r.solids == 1);

    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    f.close();

    // Non-empty and larger than the bare 84-byte header.
    CHECK(bytes.size() > 84);

    // A closed box tessellates to exactly 12 triangles (2 per planar face * 6
    // faces) at any deflection.
    const std::uint32_t tris = binaryStlTriangleCount(bytes);
    CHECK(tris == 12u);

    // Binary payload size must match the declared triangle count:
    // 84-byte header + 50 bytes per facet.
    CHECK(bytes.size() == static_cast<int>(84 + tris * 50));
}

TEST_CASE("exportStl writes an ASCII STL with the solid header", "[stl]")
{
    Rig rig;
    buildBox(rig);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("box_ascii.stl"));

    const StlResult r = exportStl(rig.doc, path, 0.1, /*ascii=*/true);
    REQUIRE(r.ok);

    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QByteArray bytes = f.readAll();
    f.close();

    CHECK(bytes.size() > 0);
    // ASCII STL starts with the "solid" keyword and contains facets.
    CHECK(bytes.startsWith("solid"));
    const int facets = bytes.count("facet normal");
    CHECK(facets == 12);
}

TEST_CASE("exportStl fails cleanly when there are no solids", "[stl]")
{
    Document doc;
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("empty.stl"));

    const StlResult r = exportStl(doc, path, 0.1, false);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.isEmpty());
}
