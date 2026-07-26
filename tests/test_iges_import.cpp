#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <BRepPrimAPI_MakeBox.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Writer.hxx>

#include "io/StepIo.h"
#include "solid/SolidEntity.h"

using namespace viki;
using Catch::Approx;

namespace {

// Writes a 20x30x40 box to an IGES file with OCCT's own writer. IGES has no
// real solid representation in common use: the writer emits the FACES, so
// reading it back exercises exactly the surface-model path real .igs files
// (vendor 3D models) take.
QString writeBoxIges(const QTemporaryDir& dir)
{
    const QString path = dir.path() + QStringLiteral("/box.igs");
    IGESControl_Controller::Init();
    IGESControl_Writer writer;
    writer.AddShape(BRepPrimAPI_MakeBox(20.0, 30.0, 40.0).Shape());
    writer.ComputeModel();
    REQUIRE(writer.Write(path.toUtf8().constData()));
    return path;
}

} // namespace

TEST_CASE("IGES import: OCCT-written box reads back as a viewable shape", "[iges]")
{
    QTemporaryDir dir;
    const QString path = writeBoxIges(dir);

    std::unique_ptr<Document> doc;
    const StepResult r = importIges(path, doc);
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    REQUIRE(doc);
    CHECK(r.solids >= 1);
    CHECK(doc->entityCount() >= 1);

    // Whatever the topology (solid or sewn faces), the geometry must span
    // the box: check the world-XY footprint of the imported shape(s).
    BBox2d box;
    for (const EntityId id : doc->drawOrder()) {
        const auto* s = dynamic_cast<const SolidEntity*>(doc->entity(id));
        REQUIRE(s);
        box.expand(s->bounds());
    }
    CHECK(box.width() == Approx(20.0).margin(0.1));
    CHECK(box.height() == Approx(30.0).margin(0.1));
}

TEST_CASE("IGES import refuses garbage with a clear error", "[iges]")
{
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/junk.igs");
    {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("this is not an IGES file at all\n");
    }
    std::unique_ptr<Document> doc;
    const StepResult r = importIges(path, doc);
    CHECK_FALSE(r.ok);
    CHECK(r.error.contains(QStringLiteral("IGES")));
}

TEST_CASE("IGES import: real vendor model from the vault", "[iges]")
{
    // ESP32 module 3D model downloaded into the C1P0 project.
    const QString real = QStringLiteral(
        "/home/lex/LSB_LexSecondBrain/_1_Projets/C1P0_ErabliereIoT/CAD/"
        "Download/esp32-wifi-lora-1.snapshot.15");
    QString found;
    if (QFileInfo::exists(real)) {
        const QDir d(real);
        for (const QFileInfo& fi :
             d.entryInfoList({QStringLiteral("*.igs"), QStringLiteral("*.iges")},
                             QDir::Files)) {
            found = fi.absoluteFilePath();
            break;
        }
    }
    if (found.isEmpty()) {
        SKIP("no real .igs present on this machine");
    }
    std::unique_ptr<Document> doc;
    const StepResult r = importIges(found, doc);
    INFO(found.toStdString());
    INFO(r.error.toStdString());
    REQUIRE(r.ok);
    CHECK(doc->entityCount() >= 1);
}
