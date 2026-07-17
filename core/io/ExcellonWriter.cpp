#include "ExcellonWriter.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include <QFile>
#include <QJsonObject>
#include <QSet>

#include "Version.h"
#include "doc/Entities.h"

namespace viki {
namespace {

// Same guard as the Gerber writer (%FSLAX46): 4 integer digits of range.
constexpr double kMaxCoordMm = 9999.999999;
// Tools are distinct when their diameters differ at 1e-4 mm (0.1 um — far
// below any drill tolerance; 1e-6-scale noise from edits never forks a tool).
constexpr double kDiaKeyScale = 1e4;

// mm value as an explicit decimal: 6 decimals (the writer's resolution),
// trailing zeros trimmed but ALWAYS keeping the point and one digit — a
// bare integer would fall back to the consumer's zero-suppression
// arithmetic, the exact ambiguity this dialect exists to avoid.
QString fmtMm(double v)
{
    double r = std::round(v * 1e6) / 1e6;
    if (r == 0.0)
        r = 0.0; // normalize -0
    QString s = QString::number(r, 'f', 6);
    while (s.endsWith(QLatin1Char('0')))
        s.chop(1);
    if (s.endsWith(QLatin1Char('.')))
        s += QLatin1Char('0');
    return s;
}

struct Hole {
    Vec2d pos;
    double dia = 0.0;
    bool plated = true;
};

struct Tool {
    int number = 0;
    double dia = 0.0; // representative: first-encountered circle's diameter
    bool plated = true;
    std::vector<const Hole*> hits; // document draw order
};

} // namespace

ExcellonExportResult writeExcellon(const Document& doc,
                                   const QStringList& layerNames, QByteArray& out)
{
    ExcellonExportResult res;

    // ---- resolve the layer set (explicit names, or every Drill-role layer)
    std::vector<const Layer*> layers;
    QSet<LayerId> seen;
    const auto addLayer = [&](const Layer& l) {
        if (seen.contains(l.id))
            return;
        seen.insert(l.id);
        layers.push_back(&l);
    };
    if (layerNames.isEmpty()) {
        for (const Layer& l : doc.layers())
            if (l.gerberRole == QLatin1String("Drill") ||
                l.gerberRole == QLatin1String("Drill-NPTH"))
                addLayer(l);
        if (layers.empty()) {
            res.error = QStringLiteral(
                "no layer has the Drill or Drill-NPTH role (name the layers "
                "to export explicitly)");
            return res;
        }
    } else {
        for (const QString& name : layerNames) {
            const Layer* found = nullptr;
            for (const Layer& l : doc.layers())
                if (l.name == name) {
                    found = &l;
                    break;
                }
            if (!found) {
                res.error = QStringLiteral("no layer named '%1'").arg(name);
                return res;
            }
            addLayer(*found);
        }
    }

    // ---- gather the holes (document draw order = stable) -------------------
    std::map<LayerId, const Layer*> byId;
    for (const Layer* l : layers) {
        byId[l->id] = l;
        if (!l->camMeta.value(QLatin1String("apertures")).toObject().isEmpty())
            res.warnings << QStringLiteral(
                              "layer '%1' carries a Gerber aperture table — only "
                              "its circles export as drill hits (graphics belong "
                              "in the Gerber export)")
                              .arg(l->name);
    }
    std::vector<Hole> holes;
    std::map<QString, int> skippedByType; // "layer/type" -> count
    for (const EntityId id : doc.drawOrder()) {
        const Entity* e = doc.entity(id);
        if (!e)
            continue;
        const auto it = byId.find(e->layerId());
        if (it == byId.end())
            continue;
        const auto* c = dynamic_cast<const CircleEntity*>(e);
        if (!c) {
            ++skippedByType[it->second->name + QLatin1Char('/') +
                            QLatin1String(e->typeName())];
            ++res.skipped;
            continue;
        }
        Hole h;
        h.pos = c->center();
        h.dia = c->radius() * 2.0;
        // Plating: the importer's tag when present, else the layer's role.
        h.plated = c->extra().value(QLatin1String("plated"))
                       .toBool(it->second->gerberRole !=
                               QLatin1String("Drill-NPTH"));
        if (h.dia <= 0.0) {
            ++skippedByType[it->second->name + QLatin1String("/zero-diameter")];
            ++res.skipped;
            continue;
        }
        if (std::fabs(h.pos.x) > kMaxCoordMm + 1e-9 ||
            std::fabs(h.pos.y) > kMaxCoordMm + 1e-9) {
            res.error = QStringLiteral(
                            "hole at (%1, %2) mm exceeds the coordinate range "
                            "(+-9999.999999)")
                            .arg(h.pos.x)
                            .arg(h.pos.y);
            return res;
        }
        holes.push_back(h);
    }
    for (const auto& [key, n] : skippedByType)
        res.warnings << QStringLiteral(
                          "%1 entity(ies) '%2' have no drill image — skipped")
                          .arg(n)
                          .arg(key);

    // ---- regenerate the tool table -----------------------------------------
    // Key = (diameter rounded to 1e-4 mm, plated). Numbering: plated tools
    // first, then NPTH, ascending diameter inside each group (Altium's shape).
    std::map<std::pair<qint64, bool>, Tool> table;
    for (const Hole& h : holes) {
        const std::pair<qint64, bool> key{qint64(std::llround(h.dia * kDiaKeyScale)),
                                          h.plated};
        Tool& t = table[key];
        if (t.hits.empty()) {
            t.dia = h.dia; // first-encountered diameter, full precision
            t.plated = h.plated;
        }
        t.hits.push_back(&h);
    }
    std::vector<Tool*> order;
    order.reserve(table.size());
    for (auto& [key, t] : table)
        order.push_back(&t);
    std::stable_sort(order.begin(), order.end(), [](const Tool* a, const Tool* b) {
        if (a->plated != b->plated)
            return a->plated; // plated group first
        return a->dia < b->dia;
    });
    int next = 1;
    for (Tool* t : order)
        t->number = next++;

    if (holes.empty())
        res.warnings << QStringLiteral(
            "no drill hits on the selected layer(s) — writing an empty "
            "(header-only) file");

    // ---- serialize ----------------------------------------------------------
    QStringList f;
    f << QStringLiteral("M48");
    f << QStringLiteral(";GenerationSoftware,VikiCAD,") +
             QLatin1String(versionString());
    f << QStringLiteral("METRIC,TZ");
    bool inPlated = false, inNpth = false;
    for (const Tool* t : order) {
        if (t->plated && !inPlated) {
            f << QStringLiteral(";TYPE=PLATED");
            inPlated = true;
        } else if (!t->plated && !inNpth) {
            f << QStringLiteral(";TYPE=NON_PLATED");
            inNpth = true;
        }
        f << QStringLiteral("T%1C%2").arg(t->number).arg(fmtMm(t->dia));
    }
    f << QStringLiteral("%");

    for (const Tool* t : order) {
        f << QStringLiteral("T%1").arg(t->number);
        // Modality resets at every tool change: the first hit restates X and Y.
        QString lastX, lastY;
        for (const Hole* h : t->hits) {
            const QString x = fmtMm(h->pos.x);
            const QString y = fmtMm(h->pos.y);
            QString line;
            if (x != lastX)
                line += QStringLiteral("X") + x;
            if (y != lastY)
                line += QStringLiteral("Y") + y;
            if (line.isEmpty()) // duplicate hole: restate both (a line needs
                line = QStringLiteral("X") + x + QStringLiteral("Y") + y; // coords)
            f << line;
            lastX = x;
            lastY = y;
            ++res.holes;
        }
    }
    f << QStringLiteral("M30");

    out = (f.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();
    res.tools = int(order.size());
    res.ok = true;
    return res;
}

ExcellonExportResult exportExcellon(const Document& doc,
                                    const QStringList& layerNames,
                                    const QString& path)
{
    QByteArray bytes;
    ExcellonExportResult res = writeExcellon(doc, layerNames, bytes);
    if (!res.ok)
        return res;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        res.ok = false;
        res.error = QStringLiteral("cannot write '%1'").arg(path);
        return res;
    }
    if (f.write(bytes) != bytes.size()) {
        res.ok = false;
        res.error = QStringLiteral("short write to '%1'").arg(path);
    }
    return res;
}

} // namespace viki
