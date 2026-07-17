#include "CommandProcessor.h"

#include <algorithm>
#include <map>
#include <vector>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "geom/GeomUtil.h"

// CAM inspection commands (G2) — read-only reports over the metadata the
// fab-file importers stored (entity "dcode"/"tool" tags + layer camMeta):
//
//   APERTURES [layer]   the aperture table of one Gerber layer (or of every
//                       layer that has one): D-code, shape, mm parameters,
//                       usage count — aligned text + one JSON trailer.
//   DRILLREPORT         hole table by diameter over the LIVE drill circles
//                       (grouped diameter + plating, counts, total), checked
//                       against the .DRR reports of the reference kits in
//                       the test suite — aligned text + one JSON trailer.
//
// And CAM editing commands (G3):
//
//   PLWIDTH w [ids]     set the stroke width (mm) of polylines — THE trace
//                       edit; the RS-274X writer then regenerates a C,<w>
//                       aperture for the edited traces.
//   PANELIZE c r px py  duplicate the fab content (every layer with a CAM
//                       role) into a c x r grid at the given pitches — one
//                       transaction, one undo. v1: no rails/mousebites.
//
// Results go through ctx.info(): GUI history bar, CLI/IPC "messages" array
// (single CommandProcessor = automatic parity). Like MINDIST, the LAST
// message starting with '{' is the machine-readable trailer.

namespace viki {
namespace {

// One aligned text line per table row: pad every column to its width.
QStringList alignRows(const std::vector<QStringList>& rows)
{
    std::vector<int> width;
    for (const QStringList& r : rows)
        for (int i = 0; i < r.size(); ++i) {
            if (int(width.size()) <= i)
                width.push_back(0);
            width[size_t(i)] = std::max(width[size_t(i)], int(r[i].size()));
        }
    QStringList out;
    for (const QStringList& r : rows) {
        QString line;
        for (int i = 0; i < r.size(); ++i) {
            if (i)
                line += QStringLiteral("  ");
            line += i + 1 < r.size() ? r[i].leftJustified(width[size_t(i)])
                                     : r[i]; // last column: no trailing pad
        }
        out << line;
    }
    return out;
}

// APERTURES [layer] — the aperture table the Gerber importer persisted in
// the layer's camMeta. No layer name (Enter) = every layer that has one.
class AperturesCommand : public Command {
public:
    const char* name() const override { return "APERTURES"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Keyword,
                          QStringLiteral("Layer name <all gerber layers>:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        QString wanted;
        if (v.kind == InputValue::Kind::Keyword ||
            v.kind == InputValue::Kind::Text)
            wanted = v.text;
        else if (v.kind != InputValue::Kind::Finish)
            return Step::cancelled();

        if (!wanted.isEmpty() && !ctx.doc().layerByName(wanted)) {
            ctx.info(QStringLiteral("no layer named '%1'").arg(wanted));
            return Step::done();
        }

        int reported = 0;
        QJsonObject trailer;
        for (const Layer& l : ctx.doc().layers()) {
            if (!wanted.isEmpty() &&
                l.name.compare(wanted, Qt::CaseInsensitive) != 0)
                continue;
            const QJsonObject table =
                l.camMeta.value(QLatin1String("apertures")).toObject();
            if (table.isEmpty()) {
                if (!wanted.isEmpty())
                    ctx.info(QStringLiteral(
                                 "layer '%1' has no aperture table (not an "
                                 "imported Gerber layer; drills: DRILLREPORT)")
                                 .arg(l.name));
                continue;
            }
            report(ctx, l, table);
            trailer[l.name] = table;
            ++reported;
        }
        if (reported == 0 && wanted.isEmpty())
            ctx.info(QStringLiteral("no layer carries an aperture table "
                                    "(import a Gerber file first)"));
        if (!trailer.isEmpty())
            ctx.info(QString::fromUtf8(
                QJsonDocument(QJsonObject{{QStringLiteral("apertures"), trailer}})
                    .toJson(QJsonDocument::Compact)));
        return Step::done();
    }

private:
    static void report(CommandContext& ctx, const Layer& l,
                       const QJsonObject& table)
    {
        // Sort by D-code number ("D10" < "D102" numerically).
        QStringList codes = table.keys();
        std::sort(codes.begin(), codes.end(),
                  [](const QString& a, const QString& b) {
                      return a.mid(1).toInt() < b.mid(1).toInt();
                  });
        int used = 0;
        std::vector<QStringList> rows;
        for (const QString& code : codes) {
            const QJsonObject ap = table.value(code).toObject();
            const int usage = ap.value(QLatin1String("usage")).toInt();
            if (usage > 0)
                ++used;
            rows.push_back({QStringLiteral("  ") + code,
                            ap.value(QLatin1String("desc")).toString(),
                            QStringLiteral("uses %1").arg(usage)});
        }
        ctx.info(QStringLiteral("apertures on '%1' (%2 D-code(s), %3 used)%4")
                     .arg(l.name)
                     .arg(codes.size())
                     .arg(used)
                     .arg(l.gerberRole.isEmpty()
                              ? QString()
                              : QStringLiteral("  [role %1]").arg(l.gerberRole)));
        for (const QString& line : alignRows(rows))
            ctx.info(line);
    }
};

// DRILLREPORT — hole table by diameter+plating over the LIVE drill circles
// (entities tagged "tool" by the Excellon importer; the declared diameter
// comes from the layer's camMeta tool table, the counted holes from the
// document, so an erased hole leaves the report immediately).
class DrillReportCommand : public Command {
public:
    const char* name() const override { return "DRILLREPORT"; }

    Step start(CommandContext& ctx) override
    {
        struct Row {
            double dia = 0.0;
            bool plated = true;
            int count = 0;
            std::map<QString, int> tools; // "T1" -> hits (per source tool)
            QStringList layers;
        };
        // Group by (diameter rounded to 1e-6 mm, plated) — the .DRR shape.
        // A circle counts as a hole when the Excellon importer tagged it
        // (tool) OR when it lives on a Drill-role layer — a freshly DRAWN
        // hole (LAYER Drill CURRENT + CIRCLE) must show up here exactly like
        // the Excellon writer will export it. Plating follows the writer's
        // rule: the tag when present, else the layer role (Drill-NPTH =>
        // non-plated, anything else => plated).
        std::map<std::pair<int64_t, bool>, Row> rows;
        for (const EntityId id : ctx.doc().drawOrder()) {
            const auto* c = dynamic_cast<const CircleEntity*>(ctx.doc().entity(id));
            if (!c)
                continue;
            const QJsonObject& extra = c->extra();
            const QString tool = extra.value(QLatin1String("tool")).toString();
            const Layer* layer = ctx.doc().layer(c->layerId());
            const bool drillLayer =
                layer && (layer->gerberRole == QLatin1String("Drill") ||
                          layer->gerberRole == QLatin1String("Drill-NPTH"));
            if (tool.isEmpty() && !drillLayer)
                continue; // not a drill hit
            const double dia = c->radius() * 2.0;
            const bool plated =
                extra.contains(QLatin1String("plated"))
                    ? extra.value(QLatin1String("plated")).toBool()
                    : (layer &&
                       layer->gerberRole != QLatin1String("Drill-NPTH"));
            Row& row = rows[{int64_t(std::llround(dia * 1e6)), plated}];
            row.dia = dia;
            row.plated = plated;
            row.count += 1;
            // Untagged = drawn in VikiCAD: the writer regenerates a tool for
            // it at export — label it "new" rather than a bogus Tn.
            row.tools[tool.isEmpty() ? QStringLiteral("new") : tool] += 1;
            if (const Layer* l = ctx.doc().layer(c->layerId());
                l && !row.layers.contains(l->name))
                row.layers << l->name;
        }
        if (rows.empty()) {
            ctx.info(QStringLiteral("no drill hits in the document (open an "
                                    "Excellon file or a Gerber kit first)"));
            return Step::done();
        }

        int total = 0, platedTotal = 0;
        QJsonArray jrows;
        std::vector<QStringList> lines;
        for (const auto& [key, row] : rows) {
            total += row.count;
            if (row.plated)
                platedTotal += row.count;
            QStringList tools;
            for (const auto& [t, n] : row.tools)
                tools << t;
            lines.push_back(
                {QStringLiteral("  d=%1 mm").arg(QString::number(row.dia, 'f', 3)),
                 row.plated ? QStringLiteral("plated") : QStringLiteral("NPTH"),
                 QStringLiteral("%1 hole(s)").arg(row.count),
                 tools.join(QLatin1Char('+')) +
                     QStringLiteral(" on ") + row.layers.join(QLatin1Char('+'))});
            jrows.append(QJsonObject{
                {QStringLiteral("dia"), row.dia},
                {QStringLiteral("plated"), row.plated},
                {QStringLiteral("count"), row.count},
                {QStringLiteral("tools"), QJsonArray::fromStringList(tools)},
                {QStringLiteral("layers"),
                 QJsonArray::fromStringList(row.layers)}});
        }
        ctx.info(QStringLiteral("drill report: %1 hole(s) — %2 plated, %3 "
                                "NPTH, %4 diameter group(s)")
                     .arg(total)
                     .arg(platedTotal)
                     .arg(total - platedTotal)
                     .arg(rows.size()));
        for (const QString& line : alignRows(lines))
            ctx.info(line);
        ctx.info(QString::fromUtf8(
            QJsonDocument(
                QJsonObject{{QStringLiteral("drillreport"),
                             QJsonObject{{QStringLiteral("rows"), jrows},
                                         {QStringLiteral("total"), total},
                                         {QStringLiteral("plated"), platedTotal},
                                         {QStringLiteral("npth"),
                                          total - platedTotal}}}})
                .toJson(QJsonDocument::Compact)));
        return Step::done();
    }

    Step onInput(CommandContext&, const InputValue&) override
    {
        return Step::done();
    }
};

// PLWIDTH <width mm> [ids...] — set the stroke width of polylines (traces).
// Numeric parameter BEFORE the greedy entity set (project grammar law); a
// pre-selected set (SELECT) is honored. The Gerber writer regenerates a
// C,<width> aperture for edited traces, so this IS the CAM trace edit.
class PlWidthCommand : public Command {
public:
    const char* name() const override { return "PLWIDTH"; }

    Step start(CommandContext& ctx) override
    {
        if (!ctx.selection().isEmpty()) {
            m_ids = ctx.selection().ids();
            ctx.selection().clear();
        }
        return Step::cont(InputKind::Distance,
                          QStringLiteral("New width (mm):"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (!m_haveWidth) {
            if (v.kind != InputValue::Kind::Number)
                return Step::cancelled();
            if (v.number < 0.0) {
                ctx.info(QStringLiteral("width must be >= 0"));
                return Step::cancelled();
            }
            m_width = v.number;
            m_haveWidth = true;
            if (!m_ids.empty())
                return apply(ctx);
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select polylines:"));
        }
        switch (v.kind) {
        case InputValue::Kind::EntitySet:
            m_ids = v.entitySet;
            return apply(ctx);
        case InputValue::Kind::EntityRef:
            m_ids.push_back(v.entityRef);
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select polylines:"));
        case InputValue::Kind::Finish:
            return m_ids.empty() ? Step::cancelled() : apply(ctx);
        default:
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select polylines:"));
        }
    }

private:
    Step apply(CommandContext& ctx)
    {
        int n = 0, notPl = 0;
        TransactionScope tx(ctx.doc(), QStringLiteral("PLWIDTH"));
        for (const EntityId id : m_ids) {
            auto* pl = dynamic_cast<PolylineEntity*>(ctx.doc().beginModify(id));
            if (!pl) {
                if (ctx.doc().entity(id))
                    ++notPl;
                continue;
            }
            pl->setWidth(m_width);
            ctx.doc().endModify(id);
            ++n;
        }
        tx.commit();
        ctx.info(QStringLiteral("width %1 mm set on %2 polyline(s)%3")
                     .arg(m_width)
                     .arg(n)
                     .arg(notPl ? QStringLiteral(" — %1 non-polyline(s) skipped")
                                      .arg(notPl)
                                : QString()));
        return Step::done();
    }

    std::vector<EntityId> m_ids;
    double m_width = 0.0;
    bool m_haveWidth = false;
};

// PANELIZE <cols> <rows> <pitchX mm> <pitchY mm> — duplicate the document's
// fab content (every entity on a layer with a CAM role, drills included)
// into a cols x rows grid. The original board is cell (0,0); every other
// cell gets a translated CLONE (tags dcode/gpol/tool/plated ride along, so
// exports and DRILLREPORT see the whole panel). ONE transaction = one undo.
// v1 keeps it simple: no rails, no mousebites, no %SR (PCB_CAM debt) —
// pitches smaller than the board size overlap the cells (caveat emptor).
class PanelizeCommand : public Command {
public:
    const char* name() const override { return "PANELIZE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Number, QStringLiteral("Columns:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (v.kind != InputValue::Kind::Number)
            return Step::cancelled();
        switch (m_stage) {
        case 0:
            m_cols = int(std::llround(v.number));
            if (m_cols < 1) {
                ctx.info(QStringLiteral("columns must be >= 1"));
                return Step::cancelled();
            }
            ++m_stage;
            return Step::cont(InputKind::Number, QStringLiteral("Rows:"));
        case 1:
            m_rows = int(std::llround(v.number));
            if (m_rows < 1) {
                ctx.info(QStringLiteral("rows must be >= 1"));
                return Step::cancelled();
            }
            ++m_stage;
            return Step::cont(InputKind::Distance,
                              QStringLiteral("Pitch X (mm):"));
        case 2:
            m_pitchX = v.number;
            if (m_pitchX <= 0.0) {
                ctx.info(QStringLiteral("pitch X must be > 0"));
                return Step::cancelled();
            }
            ++m_stage;
            return Step::cont(InputKind::Distance,
                              QStringLiteral("Pitch Y (mm):"));
        default:
            m_pitchY = v.number;
            if (m_pitchY <= 0.0) {
                ctx.info(QStringLiteral("pitch Y must be > 0"));
                return Step::cancelled();
            }
            return run(ctx);
        }
    }

private:
    Step run(CommandContext& ctx)
    {
        // The fab content: entities on layers that carry a CAM role.
        std::vector<EntityId> src;
        for (const EntityId id : ctx.doc().drawOrder()) {
            const Entity* e = ctx.doc().entity(id);
            if (!e)
                continue;
            const Layer* l = ctx.doc().layer(e->layerId());
            if (l && !l->gerberRole.isEmpty())
                src.push_back(id);
        }
        if (src.empty()) {
            ctx.info(QStringLiteral(
                "no fab content: no layer carries a CAM role (LAYER <name> "
                "ROLE <role>, or import a Gerber kit)"));
            return Step::done();
        }
        if (m_cols * m_rows == 1) {
            ctx.info(QStringLiteral("1 x 1 panel = the board itself — "
                                    "nothing to duplicate"));
            return Step::done();
        }
        // Guard against a typo'd grid (G3 closure): PANELIZE 100 100 on a
        // real kit would clone ~23 M entities inside ONE transaction —
        // minutes of GUI freeze and gigabytes of RAM (undo doubles it).
        // 2 M cloned entities ≈ 9 s and stays undoable; beyond that the
        // command refuses with the math spelled out.
        constexpr qint64 kMaxClones = 2000000;
        const qint64 clones =
            (qint64(m_cols) * qint64(m_rows) - 1) * qint64(src.size());
        if (clones > kMaxClones) {
            ctx.info(QStringLiteral(
                         "refusing to panelize: %1 x %2 cells x %3 fab "
                         "entity(ies) = %4 clones (cap %5) — use a smaller "
                         "grid")
                         .arg(m_cols)
                         .arg(m_rows)
                         .arg(src.size())
                         .arg(clones)
                         .arg(kMaxClones));
            return Step::cancelled();
        }

        int created = 0;
        {
            TransactionScope tx(ctx.doc(), QStringLiteral("PANELIZE"));
            for (int j = 0; j < m_rows; ++j) {
                for (int i = 0; i < m_cols; ++i) {
                    if (i == 0 && j == 0)
                        continue; // the original board
                    const Xform2d xf = Xform2d::translation(
                        Vec2d(i * m_pitchX, j * m_pitchY));
                    for (const EntityId id : src) {
                        const Entity* e = ctx.doc().entity(id);
                        if (!e)
                            continue;
                        auto dup = e->clone();
                        dup->transform(xf);
                        ctx.doc().addEntity(std::move(dup));
                        ++created;
                    }
                }
            }
            tx.commit();
        }
        ctx.info(QStringLiteral(
                     "panelized %1 x %2 at pitch %3 x %4 mm: %5 fab "
                     "entity(ies) -> %6 copies (%7 new)")
                     .arg(m_cols)
                     .arg(m_rows)
                     .arg(m_pitchX)
                     .arg(m_pitchY)
                     .arg(src.size())
                     .arg(m_cols * m_rows)
                     .arg(created));
        ctx.info(QString::fromUtf8(
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("panelize"),
                     QJsonObject{{QStringLiteral("cols"), m_cols},
                                 {QStringLiteral("rows"), m_rows},
                                 {QStringLiteral("pitchx"), m_pitchX},
                                 {QStringLiteral("pitchy"), m_pitchY},
                                 {QStringLiteral("source"), qint64(src.size())},
                                 {QStringLiteral("created"), created}}}})
                .toJson(QJsonDocument::Compact)));
        return Step::done();
    }

    int m_stage = 0;
    int m_cols = 0;
    int m_rows = 0;
    double m_pitchX = 0.0;
    double m_pitchY = 0.0;
};

template <typename T>
std::unique_ptr<Command> make()
{
    return std::make_unique<T>();
}

} // namespace

void registerCamCommands(CommandProcessor& p)
{
    p.registerCommand(&make<AperturesCommand>, {QStringLiteral("APER")});
    p.registerCommand(&make<DrillReportCommand>, {QStringLiteral("DR")});
    p.registerCommand(&make<PlWidthCommand>, {QStringLiteral("PLW")});
    p.registerCommand(&make<PanelizeCommand>, {QStringLiteral("PNL")});
}

} // namespace viki
