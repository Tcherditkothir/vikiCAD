#include "GerberWriter.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>

#include "Version.h"
#include "doc/Annotations.h"
#include "doc/Block.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "geom/GeomUtil.h"
#include "io/GerberIo.h"

namespace viki {
namespace {

// %FSLAX46Y46: absolute, leading zeros omitted, 4 integer + 6 decimal digits.
constexpr double kCoordScale = 1e6;         // coordinate unit = 1e-6 mm
constexpr double kMaxCoordMm = 9999.999999; // 4 integer digits
constexpr double kParamTol = 1e-6;          // aperture dedup tolerance, mm
constexpr double kAngTol = 1e-9;            // insert rotation tolerance, rad

double roundParam(double v)
{
    const double r = std::round(v * 1e6) / 1e6;
    return r == 0.0 ? 0.0 : r; // normalize -0
}

// Aperture/macro parameter: fixed 6 decimals (= the coordinate resolution),
// trailing zeros trimmed.
QString fmtParam(double v)
{
    QString s = QString::number(roundParam(v), 'f', 6);
    while (s.endsWith(QLatin1Char('0')))
        s.chop(1);
    if (s.endsWith(QLatin1Char('.')))
        s.chop(1);
    return s;
}

// Macro names we re-emit verbatim must be valid RS-274X names.
bool validMacroName(const QString& name)
{
    if (name.isEmpty() || !name[0].isLetter())
        return false;
    for (const QChar c : name)
        if (!c.isLetterOrNumber() && c != QLatin1Char('_'))
            return false;
    return true;
}

// One camMeta aperture entry ({"shape","params","hole"?,"macro"?}), decoded.
struct CamAperture {
    bool valid = false;
    QString shape;
    std::vector<double> params;
    double hole = 0.0;
    QString macro;
};

class LayerWriter {
public:
    LayerWriter(const Document& doc, const Layer& layer) : m_doc(doc), m_layer(layer)
    {
        m_camAps = layer.camMeta.value(QLatin1String("apertures")).toObject();
        m_camMacros = layer.camMeta.value(QLatin1String("macros")).toObject();
        // Verbatim macro names own their name up front so generated outline
        // macros can never collide with one emitted later.
        for (auto it = m_camMacros.constBegin(); it != m_camMacros.constEnd(); ++it)
            m_macroNames.insert(it.key());
    }

    GerberExportResult run(QByteArray& out)
    {
        if (!m_layer.camMeta.value(QLatin1String("tools")).toObject().isEmpty())
            warn(QStringLiteral("layer '%1' is a drill layer — exported as Gerber "
                                "graphics, use an Excellon export for fabrication")
                     .arg(m_layer.name));

        for (const EntityId id : m_doc.drawOrder()) {
            const Entity* e = m_doc.entity(id);
            if (!e || e->layerId() != m_layer.id)
                continue;
            writeEntity(*e);
        }

        if (!m_rangeError.isEmpty()) {
            m_res.error = m_rangeError;
            return std::move(m_res);
        }
        if (m_res.entities == 0)
            warn(QStringLiteral("layer '%1' has no exportable entities — "
                                "writing an empty (header-only) file")
                     .arg(m_layer.name));

        QStringList f;
        f << QStringLiteral("G04 Layer %1 exported by VikiCAD %2*")
                 .arg(m_layer.name, QLatin1String(versionString()));
        f << QStringLiteral("%TF.GenerationSoftware,VikiCAD,VikiCAD,") +
                 QLatin1String(versionString()) + QStringLiteral("*%");
        f << QStringLiteral("%FSLAX46Y46*%") << QStringLiteral("%MOMM*%");
        f += m_amBlocks;
        f += m_adLines;
        f << QStringLiteral("G75*") << QStringLiteral("G01*");
        f += m_body;
        f << QStringLiteral("M02*");
        out = (f.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8();

        m_res.apertures = m_adLines.size();
        m_res.ok = true;
        return std::move(m_res);
    }

private:
    // ---- warnings (deduplicated verbatim: footprint-level messages repeat
    // for every flash of the same pad otherwise) ----------------------------
    void warn(const QString& msg)
    {
        if (m_warned.contains(msg))
            return;
        m_warned.insert(msg);
        m_res.warnings << msg;
    }

    // ---- coordinate words --------------------------------------------------
    QString coordNum(double mm)
    {
        if (std::fabs(mm) > kMaxCoordMm + 1e-9 && m_rangeError.isEmpty())
            m_rangeError = QStringLiteral(
                "coordinate %1 mm exceeds the %FSLAX46Y46 range (+-9999.999999)")
                               .arg(mm);
        return QString::number(qlonglong(std::llround(mm * kCoordScale)));
    }
    QString xy(const Vec2d& p)
    {
        return QStringLiteral("X") + coordNum(p.x) + QStringLiteral("Y") + coordNum(p.y);
    }
    QString ij(const Vec2d& v)
    {
        return QStringLiteral("I") + coordNum(v.x) + QStringLiteral("J") + coordNum(v.y);
    }

    // ---- modal state -------------------------------------------------------
    void setDark(bool dark)
    {
        if (dark == m_dark)
            return;
        m_body << (dark ? QStringLiteral("%LPD*%") : QStringLiteral("%LPC*%"));
        m_dark = dark;
    }
    void setAperture(int dcode)
    {
        if (dcode == m_curAp)
            return;
        m_body << QStringLiteral("D%1*").arg(dcode);
        m_curAp = dcode;
    }
    void setInterp(int g) // 1 linear, 2 CW, 3 CCW
    {
        if (g == m_interp)
            return;
        m_body << QStringLiteral("G0%1*").arg(g);
        m_interp = g;
    }

    // ---- aperture registry (dedup on shape + params rounded to 1e-6) ------
    int registerStd(char kind, std::vector<double> params, double hole)
    {
        for (double& v : params)
            v = roundParam(v);
        hole = roundParam(hole);
        if (hole < kParamTol)
            hole = 0.0;
        QString key = QString(QLatin1Char(kind));
        for (const double v : params)
            key += QLatin1Char(':') + fmtParam(v);
        if (hole > 0)
            key += QStringLiteral(":H") + fmtParam(hole);
        const auto it = m_dcodeByKey.constFind(key);
        if (it != m_dcodeByKey.constEnd())
            return it.value();
        const int d = m_nextDcode++;
        QStringList fields;
        for (const double v : params)
            fields << fmtParam(v);
        if (hole > 0)
            fields << fmtParam(hole);
        m_adLines << QStringLiteral("%ADD") + QString::number(d) + QLatin1Char(kind) +
                         QLatin1Char(',') + fields.join(QLatin1Char('X')) +
                         QStringLiteral("*%");
        m_dcodeByKey.insert(key, d);
        return d;
    }

    // Re-emits a %AM body VERBATIM (camMeta "macros" rows: [code, p...]).
    int registerMacro(const QString& name, const QJsonArray& prims)
    {
        const QString key = QStringLiteral("M:") + name;
        const auto it = m_dcodeByKey.constFind(key);
        if (it != m_dcodeByKey.constEnd())
            return it.value();
        QStringList lines;
        lines << QStringLiteral("AM") + name + QStringLiteral("*");
        for (const QJsonValue& pv : prims) {
            const QJsonArray row = pv.toArray();
            if (row.isEmpty())
                return -1;
            QStringList fields;
            fields << QString::number(row[0].toInt());
            for (int k = 1; k < row.size(); ++k)
                fields << fmtParam(row[k].toDouble());
            lines << fields.join(QLatin1Char(',')) + QStringLiteral("*");
        }
        if (lines.size() < 2)
            return -1;
        m_amBlocks << QLatin1Char('%') + lines.join(QLatin1Char('\n')) + QLatin1Char('%');
        const int d = m_nextDcode++;
        m_adLines << QStringLiteral("%ADD") + QString::number(d) + name +
                         QStringLiteral("*%");
        m_dcodeByKey.insert(key, d);
        m_macroNames.insert(name);
        return d;
    }

    // Fallback: the flash footprint as an outline macro (primitive 4, one per
    // ring), points relative to the flash origin. Deduplicated on geometry.
    int registerOutline(const std::vector<std::vector<Vec2d>>& rings, const QString& why)
    {
        QString key = QStringLiteral("O");
        for (const auto& ring : rings) {
            key += QLatin1Char('|');
            for (const Vec2d& p : ring)
                key += QString::number(qlonglong(std::llround(p.x * kCoordScale))) +
                       QLatin1Char(',') +
                       QString::number(qlonglong(std::llround(p.y * kCoordScale))) +
                       QLatin1Char(';');
        }
        const auto it = m_dcodeByKey.constFind(key);
        if (it != m_dcodeByKey.constEnd())
            return it.value();
        QString name;
        do
            name = QStringLiteral("VKOUT%1").arg(++m_outlineSeq);
        while (m_macroNames.contains(name));
        m_macroNames.insert(name);
        QStringList lines;
        lines << QStringLiteral("AM") + name + QStringLiteral("*");
        for (const auto& ring : rings) {
            QStringList fields;
            fields << QStringLiteral("4") << QStringLiteral("1")
                   << QString::number(ring.size());
            for (const Vec2d& p : ring)
                fields << fmtParam(p.x) << fmtParam(p.y);
            fields << fmtParam(ring.front().x) << fmtParam(ring.front().y); // close
            fields << QStringLiteral("0");                                  // rotation
            lines << fields.join(QLatin1Char(',')) + QStringLiteral("*");
        }
        m_amBlocks << QLatin1Char('%') + lines.join(QLatin1Char('\n')) + QLatin1Char('%');
        const int d = m_nextDcode++;
        m_adLines << QStringLiteral("%ADD") + QString::number(d) + name +
                         QStringLiteral("*%");
        m_dcodeByKey.insert(key, d);
        warn(QStringLiteral("aperture rebuilt as outline macro '%1' (%2)").arg(name, why));
        return d;
    }

    // ---- camMeta lookups ---------------------------------------------------
    CamAperture camAperture(int dcode) const
    {
        CamAperture out;
        const QJsonObject e =
            m_camAps.value(QStringLiteral("D%1").arg(dcode)).toObject();
        if (e.isEmpty())
            return out;
        out.shape = e.value(QLatin1String("shape")).toString();
        for (const QJsonValue& v : e.value(QLatin1String("params")).toArray())
            out.params.push_back(v.toDouble());
        out.hole = e.value(QLatin1String("hole")).toDouble(0.0);
        out.macro = e.value(QLatin1String("macro")).toString();
        out.valid = !out.shape.isEmpty();
        return out;
    }

    // The original aperture, re-registered unchanged.
    int registerCam(const CamAperture& ap)
    {
        if (ap.shape == QLatin1String("Circle") && ap.params.size() >= 1)
            return registerStd('C', {ap.params[0]}, ap.hole);
        if (ap.shape == QLatin1String("Rect") && ap.params.size() >= 2)
            return registerStd('R', {ap.params[0], ap.params[1]}, ap.hole);
        if (ap.shape == QLatin1String("Obround") && ap.params.size() >= 2)
            return registerStd('O', {ap.params[0], ap.params[1]}, ap.hole);
        if (ap.shape == QLatin1String("Polygon") && ap.params.size() >= 3)
            return registerStd('P', {ap.params[0], ap.params[1], ap.params[2]},
                               ap.hole);
        if (ap.shape == QLatin1String("Macro") && validMacroName(ap.macro)) {
            const QJsonArray prims = m_camMacros.value(ap.macro).toArray();
            if (!prims.isEmpty())
                return registerMacro(ap.macro, prims);
        }
        return -1;
    }

    // Rings of a camMeta macro, via the importer's own evaluator (so stroke
    // width checks agree with what the import baked into the block).
    std::vector<std::vector<Vec2d>> macroRings(const QString& name) const
    {
        const QJsonArray prims = m_camMacros.value(name).toArray();
        if (prims.isEmpty())
            return {};
        GerberFile tmp;
        GerberMacro macro;
        macro.name = name;
        for (const QJsonValue& pv : prims) {
            const QJsonArray row = pv.toArray();
            if (row.isEmpty())
                return {};
            GerberMacroPrim p;
            p.code = row[0].toInt();
            for (int k = 1; k < row.size(); ++k)
                p.params.push_back(row[k].toDouble());
            macro.prims.push_back(std::move(p));
        }
        tmp.macros[name] = std::move(macro);
        GerberAperture ap;
        ap.kind = 'M';
        ap.macroName = name;
        QStringList scratch;
        return gerberApertureRings(ap, tmp, scratch);
    }

    // Stroke width the importer derived from this aperture (-1 = unknown).
    double camStrokeWidth(const CamAperture& ap) const
    {
        if ((ap.shape == QLatin1String("Circle") ||
             ap.shape == QLatin1String("Polygon")) &&
            !ap.params.empty())
            return ap.params[0];
        if ((ap.shape == QLatin1String("Rect") ||
             ap.shape == QLatin1String("Obround")) &&
            ap.params.size() >= 2)
            return std::min(ap.params[0], ap.params[1]);
        if (ap.shape == QLatin1String("Macro")) {
            BBox2d box;
            for (const auto& ring : macroRings(ap.macro))
                for (const Vec2d& p : ring)
                    box.expand(p);
            if (box.isValid())
                return std::min(box.width(), box.height());
        }
        return -1.0;
    }

    // ---- entity writers ----------------------------------------------------
    void writeEntity(const Entity& e)
    {
        const bool dark =
            e.extra().value(QLatin1String("gpol")).toString() != QLatin1String("C");
        const int dcodeTag = e.extra().contains(QLatin1String("dcode"))
                                 ? e.extra().value(QLatin1String("dcode")).toInt(-1)
                                 : -1;

        if (const auto* pl = dynamic_cast<const PolylineEntity*>(&e)) {
            writeStroke(pl->vertices(), pl->isClosed(), pl->width(), dark, dcodeTag);
            return;
        }
        if (const auto* ins = dynamic_cast<const InsertEntity*>(&e)) {
            writeInsert(*ins, dark, dcodeTag);
            return;
        }
        if (const auto* h = dynamic_cast<const HatchEntity*>(&e)) {
            writeRegion(*h, dark, e.id());
            return;
        }
        if (const auto* ln = dynamic_cast<const LineEntity*>(&e)) {
            writeStroke({{ln->p1(), 0.0}, {ln->p2(), 0.0}}, false, 0.0, dark, dcodeTag);
            return;
        }
        if (const auto* arc = dynamic_cast<const ArcEntity*>(&e)) {
            writeArc(*arc, dark, dcodeTag);
            return;
        }
        if (const auto* c = dynamic_cast<const CircleEntity*>(&e)) {
            writeCircle(*c, dark, dcodeTag);
            return;
        }
        warn(QStringLiteral("entity #%1 (%2) has no Gerber image — skipped")
                 .arg(e.id())
                 .arg(QLatin1String(e.typeName())));
        ++m_res.skipped;
    }

    int traceAperture(double width, int dcodeTag)
    {
        if (width < 0)
            width = 0;
        if (dcodeTag >= 0) {
            const CamAperture ap = camAperture(dcodeTag);
            if (ap.valid) {
                const double expected = camStrokeWidth(ap);
                if (expected >= 0 && std::fabs(expected - width) < kParamTol) {
                    const int d = registerCam(ap);
                    if (d >= 0)
                        return d;
                }
            }
        }
        if (width < kParamTol)
            warn(QStringLiteral("zero-width stroke(s) exported with a zero-size "
                                "aperture (no width information)"));
        return registerStd('C', {width}, 0.0);
    }

    void writeStroke(const std::vector<PolyVertex>& vs, bool closed, double width,
                     bool dark, int dcodeTag)
    {
        if (vs.size() < 2) {
            ++m_res.skipped;
            return;
        }
        const int d = traceAperture(width, dcodeTag);
        setDark(dark);
        setAperture(d);
        m_body << xy(vs[0].pos) + QStringLiteral("D02*");
        const size_t n = vs.size();
        const size_t segCount = closed ? n : n - 1;
        for (size_t i = 0; i < segCount; ++i) {
            const PolyVertex& a = vs[i];
            const Vec2d b = vs[(i + 1) % n].pos;
            if (nearZero(a.bulge, 1e-12) || nearEqual(a.pos, b, 1e-12)) {
                setInterp(1);
                m_body << xy(b) + QStringLiteral("D01*");
                continue;
            }
            // bulge = tan(sweep/4); center on the perpendicular bisector of
            // the chord, (d/2)/tan(sweep/2) to the LEFT of travel (signed).
            const double sweep = 4.0 * std::atan(a.bulge);
            const Vec2d chord = b - a.pos;
            const Vec2d mid = (a.pos + b) * 0.5;
            const double off = chord.length() * 0.5 / std::tan(sweep * 0.5);
            const Vec2d center = mid + chord.normalized().perp() * off;
            setInterp(sweep < 0 ? 2 : 3);
            m_body << xy(b) + ij(center - a.pos) + QStringLiteral("D01*");
        }
        ++m_res.entities;
    }

    void writeArc(const ArcEntity& arc, bool dark, int dcodeTag)
    {
        const double sweep = arc.sweep();
        if (sweep >= 2.0 * M_PI - kAngTol) { // stored full circle
            const int d = traceAperture(0.0, dcodeTag);
            setDark(dark);
            setAperture(d);
            const Vec2d start = arc.center() + Vec2d{arc.radius(), 0.0};
            m_body << xy(start) + QStringLiteral("D02*");
            setInterp(3);
            m_body << xy(start) + ij(arc.center() - start) + QStringLiteral("D01*");
            ++m_res.entities;
            return;
        }
        // Split arcs over 180 degrees for numerically friendly bulges.
        std::vector<PolyVertex> vs;
        const int pieces = sweep > M_PI ? 2 : 1;
        const double step = sweep / pieces;
        for (int k = 0; k < pieces; ++k)
            vs.push_back({arc.center() +
                              Vec2d::polar(arc.radius(), arc.startAngle() + k * step),
                          std::tan(step / 4.0)});
        vs.push_back({arc.endPoint(), 0.0});
        writeStroke(vs, false, 0.0, dark, dcodeTag);
    }

    void writeCircle(const CircleEntity& c, bool dark, int dcodeTag)
    {
        const Vec2d start = c.center() + Vec2d{c.radius(), 0.0};
        // Drill hits (tagged "plated" by the Excellon importer) render as
        // FILLED disks — exported as a full-circle region, same image.
        if (c.extra().contains(QLatin1String("plated"))) {
            setDark(dark);
            m_body << QStringLiteral("G36*");
            m_body << xy(start) + QStringLiteral("D02*");
            setInterp(3);
            m_body << xy(start) + ij(c.center() - start) + QStringLiteral("D01*");
            m_body << QStringLiteral("G37*");
            ++m_res.entities;
            return;
        }
        const int d = traceAperture(0.0, dcodeTag);
        setDark(dark);
        setAperture(d);
        m_body << xy(start) + QStringLiteral("D02*");
        setInterp(3);
        m_body << xy(start) + ij(c.center() - start) + QStringLiteral("D01*");
        ++m_res.entities;
    }

    void writeInsert(const InsertEntity& ins, bool dark, int dcodeTag)
    {
        // Fold the negative-uniform case (S(-s,-s) == R(180) * S(s,s)).
        double sx = ins.scale, sy = ins.effScaleY(), rot = ins.rotation;
        if (sx < 0 && sy < 0 && nearEqual(sx, sy, 1e-9)) {
            sx = -sx;
            sy = -sy;
            rot += M_PI;
        }
        const bool uniform = sx > 1e-12 && nearEqual(sx, sy, 1e-9);

        int d = -1;
        QString why;
        if (dcodeTag >= 0) {
            const CamAperture ap = camAperture(dcodeTag);
            if (!ap.valid)
                why = QStringLiteral("no aperture table entry for D%1").arg(dcodeTag);
            else if (!uniform)
                why = QStringLiteral("non-uniform insert transform");
            else
                d = transformedStd(ap, sx, rot, why);
        } else {
            why = QStringLiteral("block '%1' has no aperture metadata")
                      .arg(ins.blockName);
        }
        if (d < 0)
            d = outlineForInsert(ins, why);
        if (d < 0) {
            ++m_res.skipped;
            return;
        }
        setDark(dark);
        setAperture(d);
        m_body << xy(ins.position) + QStringLiteral("D03*");
        ++m_res.entities;
    }

    // The original aperture with a uniform scale + rotation folded into the
    // standard template parameters, where the template can express it.
    int transformedStd(const CamAperture& ap, double s, double rot, QString& why)
    {
        if (ap.shape == QLatin1String("Circle") && ap.params.size() >= 1)
            return registerStd('C', {ap.params[0] * s}, ap.hole * s);
        if ((ap.shape == QLatin1String("Rect") ||
             ap.shape == QLatin1String("Obround")) &&
            ap.params.size() >= 2) {
            const char kind = ap.shape == QLatin1String("Rect") ? 'R' : 'O';
            const double m = std::remainder(rot, M_PI);
            if (nearZero(m, kAngTol))
                return registerStd(kind, {ap.params[0] * s, ap.params[1] * s},
                                   ap.hole * s);
            if (nearZero(std::fabs(m) - M_PI_2, kAngTol))
                return registerStd(kind, {ap.params[1] * s, ap.params[0] * s},
                                   ap.hole * s);
            why = QStringLiteral("rect/obround pad rotated off-axis");
            return -1;
        }
        if (ap.shape == QLatin1String("Polygon") && ap.params.size() >= 3)
            return registerStd('P',
                               {ap.params[0] * s, ap.params[1],
                                ap.params[2] + rot * 180.0 / M_PI},
                               ap.hole * s);
        if (ap.shape == QLatin1String("Macro")) {
            if (nearEqual(s, 1.0, 1e-9) &&
                nearZero(std::remainder(rot, 2.0 * M_PI), kAngTol)) {
                const int d = registerCam(ap);
                if (d >= 0)
                    return d;
                why = QStringLiteral("macro '%1' body not available "
                                     "(document saved before G3?)")
                          .arg(ap.macro);
                return -1;
            }
            why = QStringLiteral("macro pad flashed with scale/rotation");
            return -1;
        }
        why = QStringLiteral("unrecognized aperture shape '%1'").arg(ap.shape);
        return -1;
    }

    int outlineForInsert(const InsertEntity& ins, const QString& why)
    {
        const BlockDef* def = m_doc.blockByName(ins.blockName);
        if (!def) {
            warn(QStringLiteral("insert of missing block '%1' skipped")
                     .arg(ins.blockName));
            return -1;
        }
        const Xform2d xf = ins.insertXform(def->basePoint);
        std::vector<std::vector<Vec2d>> rings;
        int ignored = 0;
        for (const auto& sub : def->entities) {
            const auto* h = dynamic_cast<const HatchEntity*>(sub.get());
            if (!h) {
                ++ignored;
                continue;
            }
            for (const auto& ring : h->rings) {
                if (ring.size() < 3)
                    continue;
                std::vector<Vec2d> r;
                r.reserve(ring.size());
                for (const Vec2d& p : ring)
                    r.push_back(xf.apply(p) - ins.position);
                rings.push_back(std::move(r));
            }
        }
        if (ignored)
            warn(QStringLiteral("block '%1': %2 non-filled entity(ies) ignored in "
                                "the flash outline")
                     .arg(ins.blockName)
                     .arg(ignored));
        if (rings.empty()) {
            warn(QStringLiteral("flash of block '%1' has no filled contour — "
                                "skipped (%2)")
                     .arg(ins.blockName, why));
            return -1;
        }
        return registerOutline(rings, why);
    }

    void writeRegion(const HatchEntity& h, bool dark, EntityId id)
    {
        if (h.pattern != QLatin1String("SOLID")) {
            warn(QStringLiteral("hatch #%1 (pattern %2) is not a solid fill — "
                                "skipped")
                     .arg(id)
                     .arg(h.pattern));
            ++m_res.skipped;
            return;
        }
        std::vector<const std::vector<Vec2d>*> rings;
        for (const auto& ring : h.rings)
            if (ring.size() >= 3)
                rings.push_back(&ring);
        if (rings.empty()) {
            warn(QStringLiteral("hatch #%1 has no usable contour — skipped").arg(id));
            ++m_res.skipped;
            return;
        }
        setDark(dark);
        m_body << QStringLiteral("G36*");
        setInterp(1);
        for (const auto* ring : rings) {
            m_body << xy((*ring)[0]) + QStringLiteral("D02*");
            for (size_t k = 1; k < ring->size(); ++k)
                m_body << xy((*ring)[k]) + QStringLiteral("D01*");
            m_body << xy((*ring)[0]) + QStringLiteral("D01*"); // close the contour
        }
        m_body << QStringLiteral("G37*");
        ++m_res.entities;
    }

    // ---- data ---------------------------------------------------------------
    const Document& m_doc;
    const Layer& m_layer;
    QJsonObject m_camAps;
    QJsonObject m_camMacros;

    GerberExportResult m_res;
    QSet<QString> m_warned;
    QString m_rangeError;

    QMap<QString, int> m_dcodeByKey;
    QStringList m_adLines;
    QStringList m_amBlocks;
    QSet<QString> m_macroNames;
    int m_nextDcode = 10;
    int m_outlineSeq = 0;

    QStringList m_body;
    int m_curAp = 0;     // no aperture selected yet
    bool m_dark = true;  // %LPD is the initial state
    int m_interp = 1;    // the header always emits G01
};

} // namespace

GerberExportResult writeGerberLayer(const Document& doc, const QString& layerName,
                                    QByteArray& out)
{
    for (const Layer& l : doc.layers())
        if (l.name == layerName) {
            LayerWriter writer(doc, l);
            return writer.run(out);
        }
    GerberExportResult res;
    res.error = QStringLiteral("no layer named '%1'").arg(layerName);
    return res;
}

GerberExportResult exportGerberLayer(const Document& doc, const QString& layerName,
                                     const QString& path)
{
    QByteArray bytes;
    GerberExportResult res = writeGerberLayer(doc, layerName, bytes);
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
