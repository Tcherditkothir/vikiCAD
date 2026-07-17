#include "ExcellonIo.h"

#include <cmath>
#include <map>
#include <optional>

#include <QFile>
#include <QJsonObject>

#include "doc/Entities.h"

namespace viki {
namespace {

constexpr double kInchToMm = 25.4;

// ---------------------------------------------------------------------------
// Parse state
// ---------------------------------------------------------------------------

struct ParseState {
    ExcellonFile file;

    bool inHeader = false;   // between M48 and % / M95
    bool headerDone = false;
    bool platedSection = true; // current `;TYPE=...` header section
    int tool = 0;            // selected tool, 0 = none
    Vec2d cur;               // current point (FILE units until the mm pass)
    bool warnedSuppression = false;
    bool warnedSlot = false;

    bool ended = false;      // M30 / M00 seen
    QString error;

    bool fail(int line, const QString& msg)
    {
        if (error.isEmpty())
            error = QStringLiteral("line %1: %2").arg(line).arg(msg);
        return false;
    }
    void warn(int line, const QString& msg)
    {
        file.warnings << QStringLiteral("line %1: %2").arg(line).arg(msg);
    }
};

// Decodes one coordinate field. Explicit decimal points pass through; bare
// integers follow the zero-suppression mode (see ExcellonFormat). File units.
bool decodeCoord(const QString& field, const ExcellonFormat& fmt, double& out)
{
    if (field.isEmpty())
        return false;
    if (field.contains(QLatin1Char('.'))) {
        bool ok = false;
        out = field.toDouble(&ok);
        return ok;
    }
    int pos = 0;
    double sign = 1.0;
    if (field[0] == QLatin1Char('+')) {
        pos = 1;
    } else if (field[0] == QLatin1Char('-')) {
        sign = -1.0;
        pos = 1;
    }
    QString digits = field.mid(pos);
    if (digits.isEmpty())
        return false;
    const int width = fmt.intDigits + fmt.decDigits;
    if (digits.size() > width)
        return false;
    for (const QChar c : digits)
        if (!c.isDigit())
            return false;
    if (!fmt.suppressLeading) // LZ: trailing zeros omitted, pad on the right
        digits = digits.leftJustified(width, QLatin1Char('0'));
    bool ok = false;
    const qlonglong v = digits.toLongLong(&ok);
    if (!ok)
        return false;
    out = sign * double(v) / std::pow(10.0, fmt.decDigits);
    return true;
}

// Splits a body/tool line into (letter, number-text) pairs: "X94488Y-35827"
// -> {X,"94488"},{Y,"-35827"}; "T1F00S00C0.01181" -> T/F/S/C fields.
bool tokenizeFields(const QString& line, std::vector<std::pair<QChar, QString>>& out)
{
    int pos = 0;
    while (pos < line.size()) {
        const QChar c = line[pos];
        if (!c.isLetter())
            return false;
        ++pos;
        const int start = pos;
        if (pos < line.size() && (line[pos] == QLatin1Char('+') || line[pos] == QLatin1Char('-')))
            ++pos;
        while (pos < line.size() &&
               (line[pos].isDigit() || line[pos] == QLatin1Char('.')))
            ++pos;
        out.emplace_back(c.toUpper(), line.mid(start, pos - start));
    }
    return true;
}

// Applies the digit defaults of a freshly declared unit, unless an Altium
// `;FILE_FORMAT=a:b` comment already forced them.
void applyUnitDefaults(ParseState& st, ExcellonUnit u, int line)
{
    if (st.file.unit != ExcellonUnit::Unknown && st.file.unit != u)
        st.warn(line, QStringLiteral("unit redeclared; keeping the first declaration"));
    if (st.file.unit != ExcellonUnit::Unknown)
        return;
    st.file.unit = u;
    if (!st.file.format.fromComment) {
        st.file.format.intDigits = (u == ExcellonUnit::Inches) ? 2 : 3;
        st.file.format.decDigits = (u == ExcellonUnit::Inches) ? 4 : 3;
    }
}

// INCH / METRIC statements, with optional ,LZ / ,TZ and ignorable extras
// (e.g. "INCH,TZ" or "METRIC,LZ,000.000").
bool parseUnitLine(ParseState& st, const QString& line, int lineNo)
{
    const QStringList parts = line.split(QLatin1Char(','));
    applyUnitDefaults(st,
                      parts[0] == QLatin1String("INCH") ? ExcellonUnit::Inches
                                                        : ExcellonUnit::Millimeters,
                      lineNo);
    for (int i = 1; i < parts.size(); ++i) {
        const QString p = parts[i].trimmed();
        if (p == QLatin1String("TZ")) {
            st.file.format.suppressLeading = true; // trailing KEPT
            st.file.format.suppressionKnown = true;
        } else if (p == QLatin1String("LZ")) {
            st.file.format.suppressLeading = false; // leading KEPT
            st.file.format.suppressionKnown = true;
        } else if (p.contains(QLatin1Char('.'))) {
            // Explicit 00.0000-style format picture.
            const int dot = p.indexOf(QLatin1Char('.'));
            if (!st.file.format.fromComment) {
                st.file.format.intDigits = dot;
                st.file.format.decDigits = p.size() - dot - 1;
            }
        } else {
            st.warn(lineNo, QStringLiteral("unknown unit modifier '%1' ignored").arg(p));
        }
    }
    return true;
}

// Header comments that carry meaning in the Altium dialect.
bool parseComment(ParseState& st, const QString& line, int lineNo)
{
    const QString body = line.mid(1).trimmed();
    if (body.startsWith(QLatin1String("FILE_FORMAT="))) {
        const QString spec = body.mid(12);
        const int colon = spec.indexOf(QLatin1Char(':'));
        bool okA = false, okB = false;
        const int a = spec.left(colon).toInt(&okA);
        const int b = spec.mid(colon + 1).toInt(&okB);
        if (colon < 0 || !okA || !okB || a < 1 || a > 7 || b < 1 || b > 7)
            return st.fail(lineNo,
                           QStringLiteral("malformed ;FILE_FORMAT comment '%1'").arg(line));
        st.file.format.intDigits = a;
        st.file.format.decDigits = b;
        st.file.format.fromComment = true;
        return true;
    }
    if (body == QLatin1String("TYPE=PLATED")) {
        st.platedSection = true;
        return true;
    }
    if (body == QLatin1String("TYPE=NON_PLATED")) {
        st.platedSection = false;
        return true;
    }
    return true; // ordinary comment
}

// T line in the header (definition) or body (selection). A C field anywhere
// makes it a definition; a bare Tn in the body selects.
bool parseToolLine(ParseState& st, const QString& line, int lineNo)
{
    std::vector<std::pair<QChar, QString>> fields;
    if (!tokenizeFields(line, fields) || fields.empty() ||
        fields[0].first != QLatin1Char('T'))
        return st.fail(lineNo, QStringLiteral("malformed tool line '%1'").arg(line));
    bool ok = false;
    const int num = fields[0].second.toInt(&ok);
    if (!ok || num < 0 || num > 9999)
        return st.fail(lineNo, QStringLiteral("bad tool number in '%1'").arg(line));

    double diameter = 0.0;
    bool hasDia = false;
    for (size_t i = 1; i < fields.size(); ++i) {
        const QChar c = fields[i].first;
        if (c == QLatin1Char('C')) {
            bool numOk = false;
            diameter = fields[i].second.toDouble(&numOk);
            if (!numOk || diameter <= 0.0)
                return st.fail(lineNo,
                               QStringLiteral("bad tool diameter in '%1'").arg(line));
            hasDia = true;
        } else if (c == QLatin1Char('F') || c == QLatin1Char('S') ||
                   c == QLatin1Char('B') || c == QLatin1Char('H') ||
                   c == QLatin1Char('Z')) {
            // Feed / spindle speed / retract / hit count / depth: ignored.
        } else {
            st.warn(lineNo, QStringLiteral("unknown tool field '%1' in '%2' ignored")
                                .arg(c).arg(line));
        }
    }

    if (hasDia) {
        if (num == 0)
            return st.fail(lineNo, QStringLiteral("tool number 0 cannot be defined"));
        ExcellonTool t;
        t.number = num;
        t.diameter = diameter; // file units until the mm pass
        t.plated = st.platedSection;
        t.line = lineNo;
        if (st.file.tools.count(num))
            st.warn(lineNo, QStringLiteral("tool T%1 redefined").arg(num));
        st.file.tools[num] = t;
        if (st.headerDone)
            st.tool = num; // inline definition in the body also selects
        return true;
    }

    // Selection (body). In the header a bare Tn defines a diameter-less tool.
    if (num == 0) {
        st.tool = 0; // T0 = tool unload
        return true;
    }
    if (st.inHeader && !st.headerDone) {
        st.warn(lineNo, QStringLiteral("tool T%1 declared without a diameter").arg(num));
        ExcellonTool t;
        t.number = num;
        t.plated = st.platedSection;
        t.line = lineNo;
        st.file.tools[num] = t;
        return true;
    }
    if (!st.file.tools.count(num))
        return st.fail(lineNo, QStringLiteral("undefined tool T%1 selected").arg(num));
    st.tool = num;
    return true;
}

// X/Y hit line, possibly with an embedded G85 slot ("X..Y..G85X..Y..").
bool parseCoordinateLine(ParseState& st, const QString& line, int lineNo)
{
    std::vector<std::pair<QChar, QString>> fields;
    if (!tokenizeFields(line, fields))
        return st.fail(lineNo, QStringLiteral("malformed drill line '%1'").arg(line));
    if (st.file.unit == ExcellonUnit::Unknown)
        return st.fail(lineNo, QStringLiteral("coordinate before INCH/METRIC declaration"));
    if (!st.tool)
        return st.fail(lineNo, QStringLiteral("drill hit with no tool selected"));

    // First target (modal against the current point), optional second after G85.
    Vec2d target = st.cur;
    bool slot = false;
    Vec2d slotEnd;
    bool any = false;

    auto decodeInto = [&](const QString& text, const QChar axis, Vec2d& dst) -> bool {
        double v = 0.0;
        if (!decodeCoord(text, st.file.format, v))
            return st.fail(lineNo,
                           QStringLiteral("bad coordinate '%1%2'").arg(axis).arg(text));
        if (!st.file.format.suppressionKnown && !text.contains(QLatin1Char('.')) &&
            !st.warnedSuppression) {
            st.warnedSuppression = true;
            st.warn(lineNo, QStringLiteral("no LZ/TZ declared; assuming leading-zero "
                                           "suppression (TZ)"));
        }
        if (axis == QLatin1Char('X'))
            dst.x = v;
        else
            dst.y = v;
        return true;
    };

    for (const auto& [c, text] : fields) {
        if (c == QLatin1Char('G')) {
            if (text == QLatin1String("85")) {
                if (slot)
                    return st.fail(lineNo, QStringLiteral("multiple G85 in one line"));
                slot = true;
                slotEnd = target;
                continue;
            }
            return st.fail(lineNo,
                           QStringLiteral("unexpected G%1 in drill line '%2'").arg(text, line));
        }
        if (c != QLatin1Char('X') && c != QLatin1Char('Y'))
            return st.fail(lineNo,
                           QStringLiteral("unexpected '%1' in drill line '%2'").arg(c).arg(line));
        if (!decodeInto(text, c, slot ? slotEnd : target))
            return false;
        any = true;
    }
    if (!any)
        return st.fail(lineNo, QStringLiteral("drill line without coordinates '%1'").arg(line));

    if (slot) {
        if (!st.warnedSlot) {
            st.warnedSlot = true;
            st.warn(lineNo, QStringLiteral("G85 slot(s) parsed but not drilled as hits"));
        }
        ExcellonSlot s;
        s.tool = st.tool;
        s.from = target;
        s.to = slotEnd;
        s.line = lineNo;
        st.file.drillSlots.push_back(s);
        st.cur = slotEnd;
        return true;
    }

    ExcellonHit h;
    h.tool = st.tool;
    h.pos = target;
    h.line = lineNo;
    st.file.hits.push_back(h);
    st.cur = target;
    return true;
}

bool processLine(ParseState& st, const QString& rawLine, int lineNo)
{
    const QString line = rawLine.trimmed();
    if (line.isEmpty())
        return true;
    if (line.startsWith(QLatin1Char(';')))
        return parseComment(st, line, lineNo);

    if (line == QLatin1String("M48")) {
        if (st.inHeader || st.headerDone)
            return st.fail(lineNo, QStringLiteral("unexpected second M48"));
        st.inHeader = true;
        return true;
    }
    if (line == QLatin1String("%") || line == QLatin1String("M95")) {
        if (st.inHeader) {
            st.inHeader = false;
            st.headerDone = true;
        } else {
            st.warn(lineNo, QStringLiteral("stray '%1' outside the header").arg(line));
        }
        return true;
    }
    if (line == QLatin1String("M30") || line == QLatin1String("M00")) {
        st.file.sawEnd = true;
        st.ended = true;
        return true;
    }
    if (line == QLatin1String("M71")) { // metric mode
        applyUnitDefaults(st, ExcellonUnit::Millimeters, lineNo);
        return true;
    }
    if (line == QLatin1String("M72")) { // inch mode
        applyUnitDefaults(st, ExcellonUnit::Inches, lineNo);
        return true;
    }
    if (line.startsWith(QLatin1String("INCH")) || line.startsWith(QLatin1String("METRIC")))
        return parseUnitLine(st, line, lineNo);
    if (line.startsWith(QLatin1String("FMAT")) || line.startsWith(QLatin1String("VER")))
        return true; // format revision / version — harmless
    if (line.startsWith(QLatin1Char('T')))
        return parseToolLine(st, line, lineNo);

    if (line.startsWith(QLatin1Char('R')) && line.size() > 1 && line[1].isDigit())
        return st.fail(lineNo, QStringLiteral(
            "repeat code (R) not supported — would silently multiply holes"));

    if (line.startsWith(QLatin1Char('G'))) {
        // A line may START with G85 (slot with a modal start point).
        if (line.startsWith(QLatin1String("G85")))
            return parseCoordinateLine(st, line, lineNo);
        if (line == QLatin1String("G90"))
            return true; // absolute — the only supported mode anyway
        if (line == QLatin1String("G91"))
            return st.fail(lineNo,
                           QStringLiteral("incremental coordinates (G91) not supported"));
        if (line == QLatin1String("G05") || line == QLatin1String("G81"))
            return true; // back to drill mode
        if (line.startsWith(QLatin1String("G00")) || line == QLatin1String("G01") ||
            line == QLatin1String("G02") || line == QLatin1String("G03"))
            return st.fail(lineNo, QStringLiteral(
                "route mode (G00-G03) not supported — slots would be lost"));
        st.warn(lineNo, QStringLiteral("unknown command '%1' ignored").arg(line));
        return true;
    }
    if (line.startsWith(QLatin1Char('M'))) {
        st.warn(lineNo, QStringLiteral("unknown command '%1' ignored").arg(line));
        return true;
    }
    if (line.startsWith(QLatin1Char('X')) || line.startsWith(QLatin1Char('Y')))
        return parseCoordinateLine(st, line, lineNo);

    return st.fail(lineNo, QStringLiteral("unrecognized line '%1'").arg(line));
}

// ---------------------------------------------------------------------------
// Inch -> mm pass (the ONLY place units are converted)
// ---------------------------------------------------------------------------

void convertToMm(ExcellonFile& f)
{
    if (f.unit != ExcellonUnit::Inches)
        return;
    const double s = kInchToMm;
    for (auto& [num, tool] : f.tools) {
        (void)num;
        tool.diameter *= s;
    }
    for (ExcellonHit& h : f.hits)
        h.pos = h.pos * s;
    for (ExcellonSlot& sl : f.drillSlots) {
        sl.from = sl.from * s;
        sl.to = sl.to * s;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// parseExcellon / parseExcellonData
// ---------------------------------------------------------------------------

ExcellonParseResult parseExcellonData(const QByteArray& data)
{
    ExcellonParseResult res;
    ParseState st;
    const QList<QByteArray> lines = data.split('\n');
    int lineNo = 0;
    for (const QByteArray& raw : lines) {
        ++lineNo;
        if (st.ended || !st.error.isEmpty())
            break;
        QByteArray l = raw;
        if (l.endsWith('\r'))
            l.chop(1);
        if (!processLine(st, QString::fromLatin1(l), lineNo))
            break;
    }

    if (st.error.isEmpty()) {
        if (st.inHeader)
            st.fail(lineNo, QStringLiteral("M48 header never closed ('%' or M95)"));
        else if (!st.file.sawEnd)
            st.warn(lineNo, QStringLiteral("missing M30 end-of-file"));
    }
    if (st.error.isEmpty() && st.file.unit == ExcellonUnit::Unknown &&
        (!st.file.hits.empty() || !st.file.tools.empty()))
        st.fail(1, QStringLiteral("no unit declaration (INCH/METRIC or M71/M72)"));

    if (!st.error.isEmpty()) {
        res.error = st.error;
        return res;
    }
    convertToMm(st.file);
    res.file = std::move(st.file);
    res.ok = true;
    return res;
}

ExcellonParseResult parseExcellon(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        ExcellonParseResult res;
        res.error = QStringLiteral("cannot open '%1'").arg(path);
        return res;
    }
    return parseExcellonData(f.readAll());
}

// ---------------------------------------------------------------------------
// Stage 2 — excellonToDocument
// ---------------------------------------------------------------------------

ExcellonImportResult excellonToDocument(Document& doc, const ExcellonFile& file,
                                        const QString& layerName)
{
    ExcellonImportResult res;
    if (layerName.isEmpty() || layerName.contains(QLatin1Char(' ')) ||
        layerName.contains(QLatin1Char('\t'))) {
        res.error = QStringLiteral("layer name must be a single non-empty token");
        return res;
    }
    res.warnings = file.warnings;

    // Validate every referenced tool BEFORE mutating the document.
    for (const ExcellonHit& h : file.hits) {
        const auto it = file.tools.find(h.tool);
        if (it == file.tools.end()) {
            res.error = QStringLiteral("line %1: hit uses unknown tool T%2")
                            .arg(h.line).arg(h.tool);
            return res;
        }
        if (it->second.diameter <= 0.0) {
            res.error = QStringLiteral("line %1: tool T%2 has no diameter")
                            .arg(h.line).arg(h.tool);
            return res;
        }
    }
    if (!file.drillSlots.empty())
        res.warnings << QStringLiteral("%1 G85 slot(s) not converted").arg(file.drillSlots.size());

    const LayerId layerId = doc.ensureLayer(layerName);
    // Kit imports (GerberKit) wrap several files in ONE transaction; nested
    // transactions assert, so only open our own scope when the caller has
    // none (same contract as gerberToDocument).
    std::optional<TransactionScope> tx;
    if (!doc.inTransaction())
        tx.emplace(doc, QStringLiteral("EXCELLONIMPORT"));
    std::map<int, int> usage; // hits per tool, for the layer's tool table
    for (const ExcellonHit& h : file.hits) {
        const ExcellonTool& tool = file.tools.at(h.tool);
        auto c = std::make_unique<CircleEntity>(h.pos, tool.diameter * 0.5);
        c->setLayerId(layerId);
        c->setExtraValue(QStringLiteral("plated"), tool.plated);
        // Which drill bit made this hole — "T3" like the source file. The
        // diameter stays readable on the circle itself (radius * 2).
        c->setExtraValue(QStringLiteral("tool"),
                         QStringLiteral("T%1").arg(h.tool));
        doc.addEntity(std::move(c));
        ++usage[h.tool];
        ++res.hits;
        ++res.entities;
    }

    // The TOOL TABLE becomes layer metadata (persisted in .vkd): every
    // declared tool with its mm diameter, plating and hit count — the
    // DRILLREPORT source and the future Excellon exporter's truth.
    // Re-importing onto the same layer replaces the "tools" table only.
    {
        QJsonObject table;
        for (const auto& [num, tool] : file.tools) {
            const auto used = usage.find(num);
            table[QStringLiteral("T%1").arg(num)] =
                QJsonObject{{QStringLiteral("dia"), tool.diameter},
                            {QStringLiteral("plated"), tool.plated},
                            {QStringLiteral("usage"),
                             used == usage.end() ? 0 : used->second}};
        }
        const Layer* layer = doc.layer(layerId);
        QJsonObject meta = layer ? layer->camMeta : QJsonObject();
        meta[QStringLiteral("tools")] = table;
        doc.setLayerCamMeta(layerId, meta);
    }

    if (tx)
        tx->commit();
    res.ok = true;
    return res;
}

} // namespace viki
