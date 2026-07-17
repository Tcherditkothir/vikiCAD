#include "CommandProcessor.h"

#include <algorithm>
#include <map>
#include <vector>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "doc/Entities.h"

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
        std::map<std::pair<int64_t, bool>, Row> rows;
        for (const EntityId id : ctx.doc().drawOrder()) {
            const auto* c = dynamic_cast<const CircleEntity*>(ctx.doc().entity(id));
            if (!c)
                continue;
            const QJsonObject& extra = c->extra();
            const QString tool = extra.value(QLatin1String("tool")).toString();
            if (tool.isEmpty())
                continue; // not a drill hit
            const double dia = c->radius() * 2.0;
            const bool plated = extra.value(QLatin1String("plated")).toBool();
            Row& row = rows[{int64_t(std::llround(dia * 1e6)), plated}];
            row.dia = dia;
            row.plated = plated;
            row.count += 1;
            row.tools[tool] += 1;
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
}

} // namespace viki
