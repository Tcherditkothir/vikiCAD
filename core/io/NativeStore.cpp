#include "NativeStore.h"

#include <sqlite3.h>

#include <QJsonDocument>

#include "doc/EntityFactory.h"

namespace viki {
namespace {

struct DbCloser {
    void operator()(sqlite3* db) const { sqlite3_close(db); }
};
using DbPtr = std::unique_ptr<sqlite3, DbCloser>;

bool exec(sqlite3* db, const char* sql, QString& error)
{
    char* msg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        error = QString::fromUtf8(msg ? msg : "sqlite error");
        sqlite3_free(msg);
        return false;
    }
    return true;
}

const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);
CREATE TABLE IF NOT EXISTS layers (
    id         INTEGER PRIMARY KEY,
    name       TEXT UNIQUE NOT NULL,
    color      INTEGER NOT NULL DEFAULT 0xFFFFFF,
    visible    INTEGER NOT NULL DEFAULT 1,
    locked     INTEGER NOT NULL DEFAULT 0,
    printable  INTEGER NOT NULL DEFAULT 1,
    sort       INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS dim_styles (
    name TEXT PRIMARY KEY,
    data TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS entities (
    id         INTEGER PRIMARY KEY,
    owner_kind INTEGER NOT NULL DEFAULT 0,
    owner_id   INTEGER NOT NULL DEFAULT 0,
    type       TEXT NOT NULL,
    layer_id   INTEGER NOT NULL DEFAULT 0,
    sort       INTEGER NOT NULL,
    data       TEXT NOT NULL,
    brep       BLOB
);
)sql";

} // namespace

bool NativeStore::save(const Document& doc, const QString& path, QString& error)
{
    sqlite3* rawDb = nullptr;
    if (sqlite3_open(path.toUtf8().constData(), &rawDb) != SQLITE_OK) {
        error = QStringLiteral("cannot open %1").arg(path);
        sqlite3_close(rawDb);
        return false;
    }
    DbPtr db(rawDb);

    const QString pragmas =
        QStringLiteral("PRAGMA journal_mode=WAL;"
                       "PRAGMA application_id=%1;"
                       "PRAGMA user_version=%2;")
            .arg(kApplicationId)
            .arg(kSchemaVersion);
    if (!exec(db.get(), pragmas.toUtf8().constData(), error))
        return false;
    if (!exec(db.get(), kSchema, error))
        return false;
    if (!exec(db.get(), "BEGIN;", error))
        return false;

    // Full rewrite: simple and correct at this document scale.
    if (!exec(db.get(),
               "DELETE FROM meta; DELETE FROM layers; DELETE FROM entities; "
               "DELETE FROM dim_styles;",
               error))
        return false;

    sqlite3_stmt* stmt = nullptr;

    sqlite3_prepare_v2(db.get(), "INSERT INTO meta(key,value) VALUES(?,?);", -1, &stmt, nullptr);
    const auto putMeta = [&](const char* key, const QString& value) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    };
    putMeta("display_units",
            doc.displayUnits() == DisplayUnits::Inches ? QStringLiteral("in")
                                                       : QStringLiteral("mm"));
    putMeta("next_id", QString::number(doc.nextId()));
    putMeta("current_layer", QString::number(doc.currentLayer()));
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db.get(),
                       "INSERT INTO layers(id,name,color,visible,locked,printable,sort) "
                       "VALUES(?,?,?,?,?,?,?);",
                       -1, &stmt, nullptr);
    int sort = 0;
    for (const Layer& l : doc.layers()) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, l.id);
        sqlite3_bind_text(stmt, 2, l.name.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, l.rgb);
        sqlite3_bind_int(stmt, 4, l.visible ? 1 : 0);
        sqlite3_bind_int(stmt, 5, l.locked ? 1 : 0);
        sqlite3_bind_int(stmt, 6, l.printable ? 1 : 0);
        sqlite3_bind_int(stmt, 7, sort++);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            error = QString::fromUtf8(sqlite3_errmsg(db.get()));
            sqlite3_finalize(stmt);
            return false;
        }
    }
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db.get(), "INSERT INTO dim_styles(name,data) VALUES(?,?);", -1,
                       &stmt, nullptr);
    for (const DimStyle& ds : doc.dimStyles()) {
        const QByteArray data =
            QJsonDocument(ds.toJson()).toJson(QJsonDocument::Compact);
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, ds.name.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, data.constData(), data.size(), SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db.get(),
                       "INSERT INTO entities(id,owner_kind,owner_id,type,layer_id,sort,data) "
                       "VALUES(?,0,0,?,?,?,?);",
                       -1, &stmt, nullptr);
    sort = 0;
    for (const EntityId id : doc.drawOrder()) {
        const Entity* e = doc.entity(id);
        if (!e)
            continue;
        const QByteArray data =
            QJsonDocument(e->toJson()).toJson(QJsonDocument::Compact);
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_bind_text(stmt, 2, e->typeName(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, e->layerId());
        sqlite3_bind_int(stmt, 4, sort++);
        sqlite3_bind_text(stmt, 5, data.constData(), data.size(), SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            error = QString::fromUtf8(sqlite3_errmsg(db.get()));
            sqlite3_finalize(stmt);
            return false;
        }
    }
    sqlite3_finalize(stmt);

    return exec(db.get(), "COMMIT;", error);
}

std::unique_ptr<Document> NativeStore::load(const QString& path, QString& error)
{
    sqlite3* rawDb = nullptr;
    if (sqlite3_open_v2(path.toUtf8().constData(), &rawDb, SQLITE_OPEN_READONLY, nullptr) !=
        SQLITE_OK) {
        error = QStringLiteral("cannot open %1").arg(path);
        sqlite3_close(rawDb);
        return nullptr;
    }
    DbPtr db(rawDb);

    auto doc = std::make_unique<Document>();
    doc->setFilePath(path);

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(db.get(), "SELECT key,value FROM meta;", -1, &stmt, nullptr) ==
        SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const QString key = QString::fromUtf8(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
            const QString value = QString::fromUtf8(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            if (key == QLatin1String("display_units"))
                doc->setDisplayUnits(value == QLatin1String("in") ? DisplayUnits::Inches
                                                                  : DisplayUnits::Millimeters);
            else if (key == QLatin1String("next_id"))
                doc->setNextId(value.toLongLong());
            else if (key == QLatin1String("current_layer"))
                doc->setCurrentLayer(value.toLongLong());
        }
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(db.get(),
                           "SELECT id,name,color,visible,locked,printable FROM layers "
                           "ORDER BY sort;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Layer l;
            l.id = sqlite3_column_int64(stmt, 0);
            l.name = QString::fromUtf8(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            l.rgb = uint32_t(sqlite3_column_int64(stmt, 2));
            l.visible = sqlite3_column_int(stmt, 3) != 0;
            l.locked = sqlite3_column_int(stmt, 4) != 0;
            l.printable = sqlite3_column_int(stmt, 5) != 0;
            doc->restoreLayer(l);
        }
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(db.get(), "SELECT data FROM dim_styles;", -1, &stmt, nullptr) ==
        SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const QByteArray data(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
                sqlite3_column_bytes(stmt, 0));
            doc->upsertDimStyle(
                DimStyle::fromJson(QJsonDocument::fromJson(data).object()));
        }
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(db.get(), "SELECT id,data FROM entities ORDER BY sort;", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        error = QStringLiteral("not a VikiCAD file (no entities table): %1").arg(path);
        return nullptr;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const EntityId id = sqlite3_column_int64(stmt, 0);
        const QByteArray data(
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
            sqlite3_column_bytes(stmt, 1));
        const QJsonObject obj = QJsonDocument::fromJson(data).object();
        auto entity = entityFromJson(obj);
        if (entity)
            doc->restoreEntity(std::move(entity), id);
    }
    sqlite3_finalize(stmt);

    return doc;
}

} // namespace viki
