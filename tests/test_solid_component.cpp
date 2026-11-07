#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtSql/QtSql>

#include <sqlite3.h>

#include <BRepPrimAPI_MakeBox.hxx>

#include "doc/Document.h"
#include "doc/EntityFactory.h"
#include "io/NativeStore.h"
#include "io/StepIo.h"
#include "solid/SolidEntity.h"

using namespace viki;

namespace {

// A fresh 10x10x10 solid at the origin.
std::unique_ptr<SolidEntity> boxSolid()
{
    BRepPrimAPI_MakeBox mk(10.0, 10.0, 10.0);
    return std::make_unique<SolidEntity>(mk.Shape());
}

// Issue #9 shape of the JSON: the component name sits at the TOP level (as
// scripts assembling .vkd documents by SQL write it) while geom.component is
// absent — the mirror image of what vikiCAD's own writer emits.
QJsonObject topLevelOnlyJson(const SolidEntity& solid, const QString& name)
{
    QJsonObject obj = solid.toJson();
    obj.insert(QStringLiteral("component"), name);
    QJsonObject geom = obj[QStringLiteral("geom")].toObject();
    geom.remove(QStringLiteral("component"));
    obj[QStringLiteral("geom")] = geom;
    return obj;
}

// Rewrite one entity's JSON inside a saved .vkd — the way the PSY1014 build
// script hot-patched the SQLite database. A plain UPDATE on entities.data.
bool patchEntityJson(const QString& path, EntityId id, const QJsonObject& obj)
{
    sqlite3* db = nullptr;
    if (sqlite3_open(path.toUtf8().constData(), &db) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    const QByteArray data =
        QJsonDocument(obj).toJson(QJsonDocument::Compact);
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db, "UPDATE entities SET data=? WHERE id=?;", -1,
                                 &stmt, nullptr) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_blob(stmt, 1, data.constData(), data.size(), SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ok;
}

} // namespace

TEST_CASE("a top-level component name survives the load (issue #9)",
          "[solid][store]")
{
    auto solid = boxSolid();
    const QJsonObject obj = topLevelOnlyJson(*solid, QStringLiteral("lobe-frontal-d"));

    auto restored = entityFromJson(obj);
    REQUIRE(restored);
    auto* back = dynamic_cast<SolidEntity*>(restored.get());
    REQUIRE(back);
    // The name the STEP writer used to read as empty now arrives — this is
    // exactly the field it indexes into PRODUCT.
    CHECK(back->component == QStringLiteral("lobe-frontal-d"));
}

TEST_CASE("geom.component stays canonical when both levels carry a name",
          "[solid][store]")
{
    auto solid = boxSolid();
    QJsonObject obj = solid->toJson();
    // v3-style file: canonical name present AND a stale top-level duplicate.
    obj.insert(QStringLiteral("component"), QStringLiteral("stale-top-name"));
    QJsonObject geom = obj[QStringLiteral("geom")].toObject();
    geom[QStringLiteral("component")] = QStringLiteral("canonical-geom-name");
    obj[QStringLiteral("geom")] = geom;

    auto restored = entityFromJson(obj);
    auto* back = dynamic_cast<SolidEntity*>(restored.get());
    REQUIRE(back);
    CHECK(back->component == QStringLiteral("canonical-geom-name"));
}

TEST_CASE("no name at either level: the solid stays anonymous as before",
          "[solid][store]")
{
    auto solid = boxSolid();
    QJsonObject obj = solid->toJson();
    QJsonObject geom = obj[QStringLiteral("geom")].toObject();
    geom.remove(QStringLiteral("component"));
    obj[QStringLiteral("geom")] = geom;

    auto restored = entityFromJson(obj);
    auto* back = dynamic_cast<SolidEntity*>(restored.get());
    REQUIRE(back);
    CHECK(back->component.isEmpty());
}

TEST_CASE("top-level name survives a .vkd save/load round-trip", "[store]")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("raw.vkd"));

    Document doc;
    doc.beginTransaction(QStringLiteral("add"));
    const EntityId solidId = doc.addEntity(boxSolid());
    doc.commitTransaction();

    QString error;
    REQUIRE(NativeStore::save(doc, path, error));

    // Reproduce the script-built file: patch the saved JSON so the name
    // lives ONLY at the top level, exactly like the PSY1014 v4 document.
    const auto* solid = dynamic_cast<const SolidEntity*>(doc.entity(solidId));
    REQUIRE(solid);
    REQUIRE(patchEntityJson(path, solidId,
                            topLevelOnlyJson(*solid, QStringLiteral("aqueduc-sylvius"))));

    const auto loaded = NativeStore::load(path, error);
    REQUIRE(loaded);
    const auto* back = dynamic_cast<const SolidEntity*>(loaded->entity(solidId));
    REQUIRE(back);
    CHECK(back->component == QStringLiteral("aqueduc-sylvius"));

    // Convergence: re-saving writes the name into geom.component (the
    // canonical field), and the next load still carries it.
    const QString convergedPath = dir.filePath(QStringLiteral("converged.vkd"));
    REQUIRE(NativeStore::save(*loaded, convergedPath, error));
    const auto twice = NativeStore::load(convergedPath, error);
    REQUIRE(twice);
    const auto* converged = dynamic_cast<const SolidEntity*>(twice->entity(solidId));
    REQUIRE(converged);
    CHECK(converged->component == QStringLiteral("aqueduc-sylvius"));
    const QJsonObject savedJson = converged->toJson();
    CHECK(savedJson[QStringLiteral("geom")].toObject()[QStringLiteral("component")]
              .toString() == QStringLiteral("aqueduc-sylvius"));
}

TEST_CASE("STEP export names PRODUCT from a top-level component (issue #9)",
          "[step][color]")
{
    auto solid = boxSolid();
    const QJsonObject obj = topLevelOnlyJson(*solid, QStringLiteral("bulbe"));

    Document doc;
    doc.restoreEntity(
        std::unique_ptr<Entity>(entityFromJson(obj).release()), 1);
    doc.setNextId(2);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("named.step"));
    const StepResult w = exportStep(doc, path);
    REQUIRE(w.ok);

    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QByteArray text = f.readAll();
    // The product is named — and NOT the XCAF auto-name ("SOLID"/"SHELL")
    // that an unnamed solid falls back to before the fix.
    CHECK(text.contains("PRODUCT('bulbe'"));
    CHECK_FALSE(text.contains("PRODUCT('SOLID'"));
    CHECK_FALSE(text.contains("PRODUCT('SHELL'"));
}