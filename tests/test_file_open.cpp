// "Open any file" — the File>Open dispatch by extension, shared by the GUI
// (via MainWindow::loadFile) and the offline CLI `open` verb (issue #11).
//
// Before #11 the CLI `open` was NativeStore-only: `$CLI open file.stl` died
// with "not a VikiCAD file (no entities table)" while `$CLI import file.stl`
// and the GUI's File>Open both accepted the very same file. The fix routes
// `open` through openFileAny(), which recognises the same kinds in the same
// order as File>Open. These tests pin the ROUTING table and the parity
// details an agent reads back (kind, counters, messages, component tagging),
// so the two channels cannot drift apart again without a red test.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "cmd/CommandProcessor.h"
#include "doc/Document.h"
#include "doc/SelectionSet.h"
#include "io/FileOpen.h"
#include "io/NativeStore.h"
#include "io/StepIo.h"
#include "io/StlIo.h"
#include "solid/SolidEntity.h"

using namespace viki;
using Catch::Matchers::WithinAbs;

namespace {

struct Rig {
    Document doc;
    SelectionSet selection;
    CommandContext ctx{doc, selection};
    CommandProcessor processor{ctx};
    Rig() { registerBuiltinCommands(processor); }
    bool run(const QString& s) { return processor.submit(s, true).ok; }
};

// A 10x10x10 box: RECT then EXTRUDE 10 on the rectangle's id.
void buildBox(Rig& rig)
{
    REQUIRE(rig.run(QStringLiteral("RECT 0,0 10,10")));
    EntityId rectId = kInvalidEntityId;
    for (const EntityId id : rig.doc.drawOrder())
        rectId = std::max(rectId, id);
    REQUIRE(rig.run(QStringLiteral("EXTRUDE 10 %1").arg(rectId)));
}

// Writes `data` to an absolute path (for files inside a kit directory).
QString writeFileAt(const QString& path, const QByteArray& data)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write(data) == data.size());
    f.close();
    return path;
}

QString writeFile(const QTemporaryDir& dir, const QString& name, const QByteArray& data)
{
    return writeFileAt(dir.filePath(name), data);
}

// A saved .vkd holding one 10mm box, and the same geometry as a .step / .stl.
QString writeBoxVkd(const QTemporaryDir& dir, const QString& name = QStringLiteral("box.vkd"))
{
    Rig rig;
    buildBox(rig);
    const QString path = dir.filePath(name);
    QString error;
    REQUIRE(NativeStore::save(rig.doc, path, error));
    return path;
}

QString writeBoxStep(const QTemporaryDir& dir, const QString& name = QStringLiteral("box.step"))
{
    Rig rig;
    buildBox(rig);
    const QString path = dir.filePath(name);
    REQUIRE(exportStep(rig.doc, path).ok);
    return path;
}

QString writeBoxStl(const QTemporaryDir& dir, const QString& name = QStringLiteral("box.stl"))
{
    Rig rig;
    buildBox(rig);
    const QString path = dir.filePath(name);
    REQUIRE(exportStl(rig.doc, path, 0.1, /*ascii=*/false).ok);
    return path;
}

const SolidEntity* soleSolid(const Document& doc)
{
    const SolidEntity* found = nullptr;
    for (const EntityId id : doc.drawOrder())
        if (const auto* s = dynamic_cast<const SolidEntity*>(doc.entity(id)))
            found = s;
    return found;
}

// Minimal RS-274X body: one 10 mm dark draw with a 0.2 mm round aperture.
QByteArray simpleGerber()
{
    return "G04 synthetic layer*\n"
           "%FSLAX24Y24*%\n"
           "%MOMM*%\n"
           "G01*\n"
           "%ADD10C,0.2*%\n"
           "D10*\n"
           "X0Y0D02*\n"
           "X100000Y0D01*\n"
           "M02*\n";
}

// Minimal hand-written R12 ASCII DXF with one line on layer "walls".
const char kTinyDxf[] = R"(0
SECTION
2
HEADER
9
$INSUNITS
70
4
0
ENDSEC
0
SECTION
2
TABLES
0
TABLE
2
LAYER
70
1
0
LAYER
2
walls
70
0
6
CONTINUOUS
0
ENDTAB
0
ENDSEC
0
SECTION
2
ENTITIES
0
LINE
8
walls
10
0.0
20
0.0
11
100.0
21
50.0
0
ENDSEC
0
EOF
)";

} // namespace

TEST_CASE("openFileAny: .vkd is the native store, and renames still work",
          "[fileopen][issue11]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString vkd = writeBoxVkd(dir);

    OpenFileResult r = openFileAny(vkd);
    REQUIRE(r.ok);
    CHECK(r.kind == QLatin1String("vkd"));
    REQUIRE(r.document != nullptr);
    CHECK(r.document->entityCount() == 1);
    CHECK(r.messages.isEmpty()); // a native load has nothing to report

    // The historical CLI behaviour must not break behind the new routing:
    // a .vkd renamed to something the dispatch table does not know still
    // opens through the native-store fallback (and says so).
    const QString renamed = dir.filePath(QStringLiteral("plan-2019.final"));
    REQUIRE(QFile::copy(vkd, renamed));
    r = openFileAny(renamed);
    REQUIRE(r.ok);
    CHECK(r.kind == QLatin1String("vkd"));
    CHECK(r.document->entityCount() == 1);
    CHECK_FALSE(r.importedKind());
}

TEST_CASE("openFileAny: an STL becomes a mesh, tagged and warned like File>Open",
          "[fileopen][issue11][stl]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString stl = writeBoxStl(dir, QStringLiteral("cortex-part.stl"));

    const OpenFileResult r = openFileAny(stl);
    REQUIRE(r.ok);
    CHECK(r.kind == QLatin1String("stl"));
    CHECK(r.importedKind());
    CHECK(r.solids == 1);
    CHECK(r.triangles == 12);
    REQUIRE(r.document != nullptr);
    CHECK(r.document->entityCount() == 1);

    // File>Open parity: STL carries no name, so every mesh gets the file
    // stem — the assembly tree must read the same whichever channel opened it.
    const SolidEntity* solid = soleSolid(*r.document);
    REQUIRE(solid != nullptr);
    CHECK(solid->component == QLatin1String("cortex-part"));
    CHECK(isMeshShape(solid->shape()));

    // The importer's summary AND its advisories are in `messages` (the CLI
    // `open` reply appends them verbatim), including the issue #4 warning
    // naming MESH2SOLID as the cure for a STEP export of a mesh.
    REQUIRE_FALSE(r.messages.isEmpty());
    CHECK(r.messages.first().contains(QLatin1String("12-triangle mesh")));
    REQUIRE_FALSE(r.warnings.isEmpty());
    CHECK(r.warnings.first().contains(QLatin1String("MESH2SOLID")));
}

TEST_CASE("openFileAny: a STEP names unnamed solids after the file, keeps real names",
          "[fileopen][issue11][step]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    // viKiCAD's own writer always stamps *some* PRODUCT name ("SOLID" for an
    // anonymous body), so an anonymous STEP has to be built deliberately: take
    // a real export and blank the PRODUCT fields, which is what a bare
    // exporter's output looks like on disk.
    const QString named = writeBoxStep(dir, QStringLiteral("ignored-stem.step"));
    QFile f(named);
    REQUIRE(f.open(QIODevice::ReadOnly));
    QByteArray text = f.readAll();
    f.close();
    REQUIRE(text.contains("PRODUCT('SOLID','SOLID','',"));
    text.replace("PRODUCT('SOLID','SOLID','',", "PRODUCT('','','',");
    const QString anonymous = dir.filePath(QStringLiteral("piece-42.step"));
    f.setFileName(anonymous);
    REQUIRE(f.open(QIODevice::WriteOnly));
    REQUIRE(f.write(text) == text.size());
    f.close();

    // (a) No part name in the file: the file stem is the fallback, exactly
    // like MainWindow::loadFile.
    OpenFileResult r = openFileAny(anonymous);
    REQUIRE(r.ok);
    CHECK(r.kind == QLatin1String("step"));
    CHECK(r.solids == 1);
    REQUIRE(r.document != nullptr);
    const SolidEntity* solid = soleSolid(*r.document);
    REQUIRE(solid != nullptr);
    CHECK(solid->component == QLatin1String("piece-42"));
    CHECK_FALSE(r.messages.isEmpty());

    // (b) A STEP that DOES carry a part name keeps it — overwriting would
    // throw away what XCAF just recovered, and the fallback rule is exactly
    // "only where the file carried none".
    Rig rig;
    buildBox(rig);
    for (const EntityId id : rig.doc.drawOrder())
        if (auto* s = dynamic_cast<SolidEntity*>(rig.doc.entity(id)))
            s->component = QStringLiteral("NAMED-IN-FILE");
    const QString carried = dir.filePath(QStringLiteral("has-a-name.step"));
    REQUIRE(exportStep(rig.doc, carried).ok);

    r = openFileAny(carried);
    REQUIRE(r.ok);
    REQUIRE(r.document != nullptr);
    const SolidEntity* kept = soleSolid(*r.document);
    REQUIRE(kept != nullptr);
    CHECK(kept->component == QLatin1String("NAMED-IN-FILE"));
}

TEST_CASE("openFileAny: a Gerber directory is a kit, a lone fab file too",
          "[fileopen][issue11][gerberkit]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    // Synthetic two-file kit: one copper layer + a report file that must be
    // skipped rather than imported.
    const QString kit = dir.filePath(QStringLiteral("kit"));
    REQUIRE(QDir().mkpath(kit));
    writeFileAt(kit + QStringLiteral("/board.GTL"), simpleGerber());
    writeFileAt(kit + QStringLiteral("/Status Report.Txt"), QByteArray("not fab data\n"));

    OpenFileResult r = openFileAny(kit);
    REQUIRE(r.ok);
    CHECK(r.kind == QLatin1String("gerberkit"));
    CHECK(r.files == 1);              // the report file is never a layer
    CHECK(r.entities > 0);
    CHECK(r.layers.contains(QLatin1String("Top-Copper")));
    REQUIRE_FALSE(r.messages.isEmpty());
    CHECK(r.messages.first().contains(QLatin1String("Gerber kit")));
    // The skipped report is named in the messages, so a missing layer is
    // never a silent mystery.
    CHECK(r.messages.join(QLatin1Char('\n')).contains(QLatin1String("Status Report")));

    // A lone fab file (the drill's usual .TXT name) opens as a one-file kit
    // by CONTENT sniff — the extension alone would not have said so.
    const QString loneDir = dir.filePath(QStringLiteral("lone"));
    REQUIRE(QDir().mkpath(loneDir));
    const QString lone = writeFileAt(loneDir + QStringLiteral("/drill.TXT"),
                                     QByteArray("M48\n"
                                                "INCH,TZ\n"
                                                "T1C0.03937\n"
                                                "%\n"
                                                "T1\n"
                                                "X10000Y10000\n"
                                                "M30\n"));
    r = openFileAny(lone);
    REQUIRE(r.ok);
    CHECK(r.kind == QLatin1String("gerberkit"));
    CHECK(r.files == 1);
}

TEST_CASE("openFileAny: DXF goes through its importer (when built with DXF)",
          "[fileopen][issue11][dxf]")
{
#ifdef VIKICAD_HAS_DXF
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString dxf = writeFile(dir, QStringLiteral("plan.dxf"),
                                  QByteArray(kTinyDxf));

    const OpenFileResult r = openFileAny(dxf);
    REQUIRE(r.ok);
    CHECK(r.kind == QLatin1String("dxf"));
    CHECK(r.importedKind());
    CHECK(r.imported == 1);
    CHECK(r.skipped == 0);
    CHECK(r.layers.contains(QLatin1String("walls")));
    REQUIRE(r.document != nullptr);
    CHECK(r.document->entityCount() == 1);
    CHECK_FALSE(r.messages.isEmpty());
#else
    SUCCEED("built without DXF support — the dispatch table has no .dxf arm");
#endif
}

TEST_CASE("openFileAny: failures name the file and keep the old native message",
          "[fileopen][issue11]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    // A missing path.
    OpenFileResult r = openFileAny(dir.filePath(QStringLiteral("nope.vkd")));
    CHECK_FALSE(r.ok);
    CHECK(r.document == nullptr);
    CHECK_FALSE(r.error.isEmpty());
    CHECK(r.error.contains(QLatin1String("nope.vkd")));

    // An unknown extension whose content is not native either: the error is
    // the historical native-store one, unchanged by the new dispatch.
    const QString junk = writeFile(dir, QStringLiteral("notes.txt"),
                                   QByteArray("just some text\n"));
    r = openFileAny(junk);
    CHECK_FALSE(r.ok);
    CHECK(r.error.contains(QLatin1String("not a VikiCAD file")));

    // An empty directory is refused with a clear message, not an empty doc.
    const QString empty = dir.filePath(QStringLiteral("emptykit"));
    REQUIRE(QDir().mkpath(empty));
    r = openFileAny(empty);
    CHECK_FALSE(r.ok);
    CHECK(r.error.contains(QLatin1String("empty directory")));

    // A truncated STL: the importer's own diagnostic survives the dispatch.
    const QString badStl = writeFile(dir, QStringLiteral("bad.stl"),
                                     QByteArray("this is not an STL\n"));
    r = openFileAny(badStl);
    CHECK_FALSE(r.ok);
    CHECK(r.error.contains(QLatin1String("bad.stl")));
}

TEST_CASE("openFileAny: the same file read by `import` and `open` agree",
          "[fileopen][issue11]")
{
    // The issue's core complaint was that two verbs disagreed about one file.
    // The dispatch guarantees they now produce the SAME document: same kind,
    // same counters, same component tags, same warnings.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString stl = writeBoxStl(dir, QStringLiteral("agree.stl"));

    const OpenFileResult opened = openFileAny(stl);

    std::unique_ptr<Document> importedDoc;
    const StlResult imported = importStl(stl, importedDoc);
    REQUIRE(opened.ok);
    REQUIRE(imported.ok);

    CHECK(opened.triangles == imported.triangles);
    CHECK(opened.solids == imported.solids);
    CHECK(opened.warnings == imported.warnings);
    REQUIRE(opened.document != nullptr);
    REQUIRE(importedDoc != nullptr);
    CHECK(opened.document->entityCount() == importedDoc->entityCount());

    // Component tagging is the ONE deliberate difference: the dispatch tags
    // the mesh with the file stem (File>Open parity), the raw importer does
    // not. Both solids share the shape and the bounds.
    const SolidEntity* a = soleSolid(*opened.document);
    const SolidEntity* b = soleSolid(*importedDoc);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->component == QLatin1String("agree"));
    CHECK(b->component.isEmpty());
    CHECK_THAT(a->bounds().max.x, WithinAbs(b->bounds().max.x, 1e-6));
    CHECK_THAT(a->bounds().max.y, WithinAbs(b->bounds().max.y, 1e-6));
}
