#include "NativeStore.h"

#include <sqlite3.h>

#include <QJsonDocument>

#include "doc/EntityFactory.h"
#include "solid/SolidOps.h"

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
CREATE TABLE IF NOT EXISTS layouts (
    id      INTEGER PRIMARY KEY,
    name    TEXT UNIQUE NOT NULL,
    paper_w REAL NOT NULL,
    paper_h REAL NOT NULL,
    sort    INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS viewports (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    layout_id INTEGER NOT NULL,
    x REAL, y REAL, w REAL, h REAL,
    center_x REAL, center_y REAL,
    scale REAL NOT NULL DEFAULT 1
);
CREATE TABLE IF NOT EXISTS blocks (
    id     INTEGER PRIMARY KEY,
    name   TEXT UNIQUE NOT NULL,
    base_x REAL NOT NULL DEFAULT 0,
    base_y REAL NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS dim_styles (
    name TEXT PRIMARY KEY,
    data TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS params (
    name TEXT PRIMARY KEY,
    expr TEXT NOT NULL,
    sort INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS sketches (
    id    INTEGER PRIMARY KEY,
    name  TEXT NOT NULL,
    plane TEXT NOT NULL,
    sort  INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS sketch_members (
    entity_id INTEGER PRIMARY KEY,
    sketch_id INTEGER NOT NULL
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

// Nine full-precision doubles "ox oy oz nx ny nz xx xy xz" — the same format
// as the "workplane" meta entry.
QString planeToString(const WorkPlane& wp)
{
    const auto num = [](double d) { return QString::number(d, 'g', 17); };
    return QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8 %9")
        .arg(num(wp.origin.X()), num(wp.origin.Y()), num(wp.origin.Z()),
             num(wp.normal.X()), num(wp.normal.Y()), num(wp.normal.Z()),
             num(wp.xDir.X()), num(wp.xDir.Y()), num(wp.xDir.Z()));
}

std::optional<WorkPlane> planeFromString(const QString& value)
{
    const QStringList parts = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() != 9)
        return std::nullopt;
    bool ok = true;
    double c[9];
    for (int i = 0; i < 9 && ok; ++i)
        c[i] = parts[i].toDouble(&ok);
    // Only accept non-degenerate direction vectors; gp_Dir throws on a
    // zero-magnitude vector, so guard before constructing.
    if (!ok || (c[3] * c[3] + c[4] * c[4] + c[5] * c[5]) <= 1e-18 ||
        (c[6] * c[6] + c[7] * c[7] + c[8] * c[8]) <= 1e-18)
        return std::nullopt;
    return WorkPlane{gp_Pnt(c[0], c[1], c[2]), gp_Dir(c[3], c[4], c[5]),
                     gp_Dir(c[6], c[7], c[8])};
}

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
               "DELETE FROM dim_styles; DELETE FROM blocks; "
               "DELETE FROM layouts; DELETE FROM viewports; DELETE FROM params; "
               "DELETE FROM sketches; DELETE FROM sketch_members;",
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
    // Persist the current work plane as nine full-precision doubles.
    putMeta("workplane", planeToString(documentWorkplane(doc)));
    // The currently-open sketch survives save/load (0 = none).
    putMeta("active_sketch", QString::number(doc.activeSketch()));
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db.get(),
                       "INSERT INTO sketches(id,name,plane,sort) VALUES(?,?,?,?);",
                       -1, &stmt, nullptr);
    {
        int ssort = 0;
        for (const SketchInfo& s : doc.sketches()) {
            const QByteArray plane = planeToString(s.plane).toUtf8();
            sqlite3_reset(stmt);
            sqlite3_bind_int64(stmt, 1, s.id);
            sqlite3_bind_text(stmt, 2, s.name.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, plane.constData(), plane.size(), SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 4, ssort++);
            sqlite3_step(stmt);
        }
    }
    sqlite3_finalize(stmt);

    // Membership tags of LIVE entities only (stale tags of removed entities
    // are an in-session undo convenience, not document state).
    sqlite3_prepare_v2(db.get(),
                       "INSERT INTO sketch_members(entity_id,sketch_id) VALUES(?,?);",
                       -1, &stmt, nullptr);
    for (const EntityId id : doc.drawOrder()) {
        const int64_t sk = doc.entitySketch(id);
        if (sk == 0)
            continue;
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_bind_int64(stmt, 2, sk);
        sqlite3_step(stmt);
    }
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

    sqlite3_prepare_v2(db.get(),
                       "INSERT INTO layouts(id,name,paper_w,paper_h,sort) VALUES(?,?,?,?,?);",
                       -1, &stmt, nullptr);
    {
        int lsort = 0;
        for (const Layout& l : doc.layouts()) {
            sqlite3_reset(stmt);
            sqlite3_bind_int64(stmt, 1, l.id);
            sqlite3_bind_text(stmt, 2, l.name.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, l.paperW);
            sqlite3_bind_double(stmt, 4, l.paperH);
            sqlite3_bind_int(stmt, 5, lsort++);
            sqlite3_step(stmt);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_prepare_v2(db.get(),
                       "INSERT INTO viewports(layout_id,x,y,w,h,center_x,center_y,scale) "
                       "VALUES(?,?,?,?,?,?,?,?);",
                       -1, &stmt, nullptr);
    for (const Layout& l : doc.layouts()) {
        for (const Viewport& vp : l.viewports) {
            sqlite3_reset(stmt);
            sqlite3_bind_int64(stmt, 1, l.id);
            sqlite3_bind_double(stmt, 2, vp.x);
            sqlite3_bind_double(stmt, 3, vp.y);
            sqlite3_bind_double(stmt, 4, vp.w);
            sqlite3_bind_double(stmt, 5, vp.h);
            sqlite3_bind_double(stmt, 6, vp.center.x);
            sqlite3_bind_double(stmt, 7, vp.center.y);
            sqlite3_bind_double(stmt, 8, vp.scale);
            sqlite3_step(stmt);
        }
    }
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db.get(),
                       "INSERT INTO blocks(id,name,base_x,base_y) VALUES(?,?,?,?);", -1,
                       &stmt, nullptr);
    for (const auto& blk : doc.blocks()) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, blk->id);
        sqlite3_bind_text(stmt, 2, blk->name.toUtf8().constData(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, blk->basePoint.x);
        sqlite3_bind_double(stmt, 4, blk->basePoint.y);
        sqlite3_step(stmt);
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

    sqlite3_prepare_v2(db.get(), "INSERT INTO params(name,expr,sort) VALUES(?,?,?);", -1,
                       &stmt, nullptr);
    {
        int psort = 0;
        for (const Param& pm : doc.params().params()) {
            sqlite3_reset(stmt);
            sqlite3_bind_text(stmt, 1, pm.name.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, pm.expr.toUtf8().constData(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, psort++);
            sqlite3_step(stmt);
        }
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

    // Block definition entities: owner_kind=1, auto row ids.
    sqlite3_prepare_v2(db.get(),
                       "INSERT INTO entities(owner_kind,owner_id,type,layer_id,sort,data) "
                       "VALUES(1,?,?,?,?,?);",
                       -1, &stmt, nullptr);
    for (const auto& blk : doc.blocks()) {
        int bsort = 0;
        for (const auto& e : blk->entities) {
            const QByteArray data =
                QJsonDocument(e->toJson()).toJson(QJsonDocument::Compact);
            sqlite3_reset(stmt);
            sqlite3_bind_int64(stmt, 1, blk->id);
            sqlite3_bind_text(stmt, 2, e->typeName(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 3, e->layerId());
            sqlite3_bind_int(stmt, 4, bsort++);
            sqlite3_bind_text(stmt, 5, data.constData(), data.size(), SQLITE_TRANSIENT);
            sqlite3_step(stmt);
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
    int64_t activeSketch = 0; // applied after the sketch registry is restored

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
            else if (key == QLatin1String("workplane")) {
                if (const auto wp = planeFromString(value))
                    documentWorkplane(*doc) = *wp;
            } else if (key == QLatin1String("active_sketch"))
                activeSketch = value.toLongLong();
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

    // params table may be absent in files written before this feature.
    if (sqlite3_prepare_v2(db.get(), "SELECT name,expr FROM params ORDER BY sort;", -1,
                           &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const QString name = QString::fromUtf8(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
            const QString expr = QString::fromUtf8(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            doc->params().restore(name, expr);
        }
        sqlite3_finalize(stmt);
        doc->params().reevaluate();
    }

    // sketch tables may be absent in files written before this feature.
    if (sqlite3_prepare_v2(db.get(), "SELECT id,name,plane FROM sketches ORDER BY sort;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SketchInfo info;
            info.id = sqlite3_column_int64(stmt, 0);
            info.name = QString::fromUtf8(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            if (const auto wp = planeFromString(QString::fromUtf8(
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)))))
                info.plane = *wp;
            doc->restoreSketch(info);
        }
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(db.get(), "SELECT entity_id,sketch_id FROM sketch_members;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW)
            doc->setEntitySketch(sqlite3_column_int64(stmt, 0),
                                 sqlite3_column_int64(stmt, 1));
        sqlite3_finalize(stmt);
    }
    if (activeSketch != 0 && doc->sketchById(activeSketch))
        doc->setActiveSketch(activeSketch);

    if (sqlite3_prepare_v2(db.get(),
                           "SELECT id,name,paper_w,paper_h FROM layouts ORDER BY sort;", -1,
                           &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Layout* l = doc->ensureLayout(QString::fromUtf8(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))));
            l->id = sqlite3_column_int64(stmt, 0);
            l->paperW = sqlite3_column_double(stmt, 2);
            l->paperH = sqlite3_column_double(stmt, 3);
        }
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(db.get(),
                           "SELECT layout_id,x,y,w,h,center_x,center_y,scale FROM viewports;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const int64_t lid = sqlite3_column_int64(stmt, 0);
            for (const Layout& cl : doc->layouts()) {
                if (cl.id == lid) {
                    Viewport vp;
                    vp.x = sqlite3_column_double(stmt, 1);
                    vp.y = sqlite3_column_double(stmt, 2);
                    vp.w = sqlite3_column_double(stmt, 3);
                    vp.h = sqlite3_column_double(stmt, 4);
                    vp.center = {sqlite3_column_double(stmt, 5),
                                 sqlite3_column_double(stmt, 6)};
                    vp.scale = sqlite3_column_double(stmt, 7);
                    const_cast<Layout&>(cl).viewports.push_back(vp);
                    break;
                }
            }
        }
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(db.get(),
                           "SELECT id,name,base_x,base_y FROM blocks ORDER BY id;", -1,
                           &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const int64_t bid = sqlite3_column_int64(stmt, 0);
            const QString name = QString::fromUtf8(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
            BlockDef* def = doc->createBlock(
                name, {sqlite3_column_double(stmt, 2), sqlite3_column_double(stmt, 3)});
            def->id = bid;
        }
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(db.get(),
                           "SELECT owner_id,data FROM entities WHERE owner_kind=1 "
                           "ORDER BY sort;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const int64_t bid = sqlite3_column_int64(stmt, 0);
            const QByteArray data(
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
                sqlite3_column_bytes(stmt, 1));
            auto entity = entityFromJson(QJsonDocument::fromJson(data).object());
            if (!entity)
                continue;
            for (const auto& blk : doc->blocks()) {
                if (blk->id == bid) {
                    const_cast<BlockDef*>(blk.get())->entities.push_back(std::move(entity));
                    break;
                }
            }
        }
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(db.get(),
                           "SELECT id,data FROM entities WHERE owner_kind=0 ORDER BY sort;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
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
