#include "GerberIo.h"

#include <algorithm>
#include <cmath>

#include <QFile>
#include <QJsonObject>
#include <QRegularExpression>

#include "doc/Annotations.h"
#include "doc/Block.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "geom/GeomUtil.h"

namespace viki {
namespace {

constexpr double kInchToMm = 25.4;
// Chord tolerance for tessellating aperture/region arcs into rings, mm.
constexpr double kRingTol = 0.001;

// ---------------------------------------------------------------------------
// Parse state
// ---------------------------------------------------------------------------

struct ParseState {
    GerberFile file;

    // Modal graphics state.
    int interp = 0;          // 1 linear, 2 CW arc, 3 CCW arc; 0 = unset
    int quadrant = 0;        // 74 single, 75 multi; 0 = unset
    bool dark = true;        // LPD / LPC
    int aperture = 0;        // selected D-code, 0 = none
    Vec2d cur;               // current point (FILE units until the mm pass)
    int lastOp = 0;          // 1..3, for deprecated coord-only words
    bool warnedModalOp = false;

    // Region state.
    bool inRegion = false;
    int regionLine = 0;
    std::vector<GerberContour> contours;
    bool contourOpen = false;
    GerberContour contour;   // open contour being built
    Vec2d contourStart;      // pending D02 target (next contour start)
    bool haveContourStart = false;

    bool ended = false;      // M02 seen
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

// Decodes one coordinate field according to %FS. Returns file units.
// Handles BOTH suppression modes: leading omitted (pad left, i.e. plain
// integer divided by 10^dec) and trailing omitted (pad right to full width).
// Explicit decimal points (deprecated but seen in the wild) pass through.
bool decodeCoord(const QString& field, const GerberFormat& fmt, double& out)
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
    if (!fmt.omitLeading) // trailing zeros omitted: pad on the right
        digits = digits.leftJustified(width, QLatin1Char('0'));
    bool ok = false;
    const qlonglong v = digits.toLongLong(&ok);
    if (!ok)
        return false;
    out = sign * double(v) / std::pow(10.0, fmt.decDigits);
    return true;
}

// ---------------------------------------------------------------------------
// Extended (%...%) statements
// ---------------------------------------------------------------------------

bool parseFS(ParseState& st, const QString& s, int line)
{
    // FSLAX25Y25 — L/T zero mode, A absolute (I incremental = deprecated,
    // rejected), then X<int><dec>Y<int><dec>.
    if (s.size() != 10 || s.mid(0, 2) != QLatin1String("FS") ||
        s[4] != QLatin1Char('X') || s[7] != QLatin1Char('Y'))
        return st.fail(line, QStringLiteral("malformed %FS statement '%1'").arg(s));
    const QChar zero = s[2];
    const QChar mode = s[3];
    if (zero != QLatin1Char('L') && zero != QLatin1Char('T'))
        return st.fail(line, QStringLiteral("%FS zero mode must be L or T"));
    if (mode == QLatin1Char('I'))
        return st.fail(line, QStringLiteral("incremental coordinates (FS..I) not supported"));
    if (mode != QLatin1Char('A'))
        return st.fail(line, QStringLiteral("%FS mode must be A or I"));
    const int xi = s[5].digitValue(), xd = s[6].digitValue();
    const int yi = s[8].digitValue(), yd = s[9].digitValue();
    if (xi < 0 || xd < 1 || xi > 7 || xd > 7)
        return st.fail(line, QStringLiteral("%FS digit counts out of range"));
    if (xi != yi || xd != yd)
        st.warn(line, QStringLiteral("%FS X and Y formats differ; using X format"));
    st.file.format.omitLeading = (zero == QLatin1Char('L'));
    st.file.format.intDigits = xi;
    st.file.format.decDigits = xd;
    st.file.format.valid = true;
    return true;
}

bool parseAD(ParseState& st, const QString& stmt, int line)
{
    // ADD10C,0.00787[X<hole>] | ADD15ROUNDEDRECTD15
    QString s = stmt;
    s.remove(QLatin1Char(' '));
    int pos = 2; // past "AD"
    if (pos >= s.size() || s[pos] != QLatin1Char('D'))
        return st.fail(line, QStringLiteral("malformed %AD statement '%1'").arg(stmt));
    ++pos;
    int digitEnd = pos;
    while (digitEnd < s.size() && s[digitEnd].isDigit())
        ++digitEnd;
    bool ok = false;
    const int dcode = s.mid(pos, digitEnd - pos).toInt(&ok);
    if (!ok || dcode < 10)
        return st.fail(line, QStringLiteral("aperture D-code must be >= 10 in '%1'").arg(stmt));
    const QString rest = s.mid(digitEnd);
    const int comma = rest.indexOf(QLatin1Char(','));
    const QString name = comma < 0 ? rest : rest.left(comma);
    QStringList paramTok;
    if (comma >= 0)
        paramTok = rest.mid(comma + 1).split(QLatin1Char('X'), Qt::SkipEmptyParts);
    if (name.isEmpty())
        return st.fail(line, QStringLiteral("missing aperture template in '%1'").arg(stmt));

    std::vector<double> params;
    for (const QString& t : paramTok) {
        bool numOk = false;
        params.push_back(t.toDouble(&numOk));
        if (!numOk)
            return st.fail(line, QStringLiteral("bad aperture parameter '%1'").arg(t));
    }

    GerberAperture ap;
    ap.dcode = dcode;
    const bool std1 = name.size() == 1;
    if (std1 && name[0] == QLatin1Char('C')) {
        if (params.size() < 1 || params.size() > 2)
            return st.fail(line, QStringLiteral("circle aperture takes 1-2 parameters"));
        ap.kind = 'C';
        ap.params = {params[0]};
        ap.holeDiameter = params.size() > 1 ? params[1] : 0.0;
    } else if (std1 && (name[0] == QLatin1Char('R') || name[0] == QLatin1Char('O'))) {
        if (params.size() < 2 || params.size() > 3)
            return st.fail(line, QStringLiteral("rect/obround aperture takes 2-3 parameters"));
        ap.kind = char(name[0].toLatin1());
        ap.params = {params[0], params[1]};
        ap.holeDiameter = params.size() > 2 ? params[2] : 0.0;
    } else if (std1 && name[0] == QLatin1Char('P')) {
        if (params.size() < 2 || params.size() > 4)
            return st.fail(line, QStringLiteral("polygon aperture takes 2-4 parameters"));
        const int n = int(std::lround(params[1]));
        if (n < 3 || n > 12)
            return st.fail(line, QStringLiteral("polygon aperture vertex count out of range"));
        ap.kind = 'P';
        ap.params = {params[0], double(n), params.size() > 2 ? params[2] : 0.0};
        ap.holeDiameter = params.size() > 3 ? params[3] : 0.0;
    } else {
        // Macro reference. Parameters would bind $1.. variables — out of scope.
        if (!params.empty())
            return st.fail(line,
                           QStringLiteral("macro aperture parameters ($ substitution) not "
                                          "supported: '%1'").arg(stmt));
        ap.kind = 'M';
        ap.macroName = name;
    }
    if (st.file.apertures.count(dcode))
        st.warn(line, QStringLiteral("aperture D%1 redefined").arg(dcode));
    st.file.apertures[dcode] = ap;
    return true;
}

bool parseAM(ParseState& st, const QStringList& stmts, int line)
{
    // stmts[0] = "AM<NAME>", stmts[1..] = primitives.
    const QString name = stmts[0].mid(2).trimmed();
    if (name.isEmpty())
        return st.fail(line, QStringLiteral("missing macro name in %AM"));
    GerberMacro macro;
    macro.name = name;
    for (int i = 1; i < stmts.size(); ++i) {
        QString s = stmts[i];
        s.remove(QLatin1Char(' '));
        s.remove(QLatin1Char('\t'));
        if (s.isEmpty())
            continue;
        if (s.startsWith(QLatin1Char('0')))
            continue; // primitive 0: comment
        if (s.contains(QLatin1Char('$')) || s.contains(QLatin1Char('=')))
            return st.fail(line, QStringLiteral(
                "macro '%1' uses variables/expressions — not supported").arg(name));
        const QStringList fields = s.split(QLatin1Char(','));
        bool ok = false;
        int code = fields[0].toInt(&ok);
        if (!ok)
            return st.fail(line, QStringLiteral("bad macro primitive '%1'").arg(stmts[i]));
        GerberMacroPrim prim;
        prim.line = line;
        for (int k = 1; k < fields.size(); ++k) {
            bool numOk = false;
            prim.params.push_back(fields[k].toDouble(&numOk));
            if (!numOk)
                return st.fail(line,
                               QStringLiteral("bad macro parameter '%1'").arg(fields[k]));
        }
        if (code == 2) { // deprecated alias of 20 (vector line)
            st.warn(line, QStringLiteral("macro primitive code 2 treated as 20"));
            code = 20;
        }
        const size_t np = prim.params.size();
        switch (code) {
        case 1:
            if (np < 4)
                return st.fail(line, QStringLiteral("macro circle needs 4+ parameters"));
            break;
        case 20:
            if (np < 7)
                return st.fail(line, QStringLiteral("macro vector line needs 7 parameters"));
            break;
        case 21:
            if (np < 6)
                return st.fail(line, QStringLiteral("macro center rect needs 6 parameters"));
            break;
        case 4: {
            if (np < 2)
                return st.fail(line, QStringLiteral("macro outline truncated"));
            const int n = int(std::lround(prim.params[1]));
            if (n < 1 || np < size_t(2 + 2 * (n + 1) + 1))
                return st.fail(line, QStringLiteral("macro outline truncated"));
            break;
        }
        case 5:
            if (np < 6)
                return st.fail(line, QStringLiteral("macro polygon needs 6 parameters"));
            break;
        case 6:
        case 7:
        case 22:
            break; // kept raw; conversion warns
        default:
            st.warn(line, QStringLiteral("unknown macro primitive code %1 in '%2'")
                              .arg(code).arg(name));
            break;
        }
        prim.code = code;
        macro.prims.push_back(std::move(prim));
    }
    if (st.file.macros.count(name))
        st.warn(line, QStringLiteral("macro '%1' redefined").arg(name));
    st.file.macros[name] = std::move(macro);
    return true;
}

// One X2 attribute statement ("TF.FileFunction,Copper,L1,Top"), from either
// the naked %TF...*% form or the `G04 #@! TF...` comment form.
void recordAttribute(ParseState& st, const QString& stmt)
{
    const QString kind = stmt.left(2);
    if (kind == QLatin1String("TF")) {
        const int comma = stmt.indexOf(QLatin1Char(','));
        const QString key = comma < 0 ? stmt.mid(2) : stmt.mid(2, comma - 2);
        const QString value = comma < 0 ? QString() : stmt.mid(comma + 1);
        st.file.fileAttributes[key.trimmed()] = value;
    } else {
        st.file.otherAttributes << stmt;
    }
}

bool processExtendedStatement(ParseState& st, const QString& stmtIn, int line)
{
    QString stmt = stmtIn.trimmed();
    if (stmt.isEmpty())
        return true;
    const QString head = stmt.left(2);
    QString compact = stmt;
    if (head != QLatin1String("TF") && head != QLatin1String("TA") &&
        head != QLatin1String("TO") && head != QLatin1String("TD") &&
        head != QLatin1String("IN") && head != QLatin1String("LN"))
        compact.remove(QLatin1Char(' '));

    if (head == QLatin1String("FS"))
        return parseFS(st, compact, line);
    if (head == QLatin1String("MO")) {
        const QString u = compact.mid(2);
        if (u == QLatin1String("IN"))
            st.file.unit = GerberUnit::Inches;
        else if (u == QLatin1String("MM"))
            st.file.unit = GerberUnit::Millimeters;
        else
            return st.fail(line, QStringLiteral("unknown unit '%1' in %MO").arg(u));
        return true;
    }
    if (head == QLatin1String("AD"))
        return parseAD(st, compact, line);
    if (head == QLatin1String("LP")) {
        if (compact == QLatin1String("LPD"))
            st.dark = true;
        else if (compact == QLatin1String("LPC"))
            st.dark = false;
        else
            return st.fail(line, QStringLiteral("malformed %LP statement '%1'").arg(stmt));
        return true;
    }
    if (head == QLatin1String("SR")) {
        // Step & repeat: only the identity block (or the closing %SR*%) is
        // accepted — anything else would silently multiply geometry.
        if (compact == QLatin1String("SR"))
            return true;
        static const QRegularExpression re(
            QStringLiteral("^SRX(\\d+)Y(\\d+)I[0-9.+-]+J[0-9.+-]+$"));
        const auto m = re.match(compact);
        if (m.hasMatch() && m.captured(1).toInt() == 1 && m.captured(2).toInt() == 1)
            return true;
        return st.fail(line, QStringLiteral("step & repeat (%SR) not supported"));
    }
    if (head == QLatin1String("TF") || head == QLatin1String("TA") ||
        head == QLatin1String("TO") || head == QLatin1String("TD")) {
        recordAttribute(st, stmt);
        return true;
    }
    if (head == QLatin1String("IP")) {
        if (compact == QLatin1String("IPPOS"))
            return true;
        return st.fail(line, QStringLiteral("negative image polarity (%IPNEG) not supported"));
    }
    if (head == QLatin1String("IN") || head == QLatin1String("LN"))
        return true; // deprecated image/layer names — harmless
    if (head == QLatin1String("MI") || head == QLatin1String("OF") ||
        head == QLatin1String("SF") || head == QLatin1String("IR") ||
        head == QLatin1String("AS")) {
        // Deprecated image transforms: identity is a no-op, anything else
        // would silently move all coordinates.
        static const QRegularExpression num(QStringLiteral("[AB]([0-9.+-]+)"));
        bool identity = true;
        auto it = num.globalMatch(compact);
        while (it.hasNext()) {
            const double v = it.next().captured(1).toDouble();
            if (head == QLatin1String("SF") ? !nearEqual(v, 1.0, 1e-9) : !nearZero(v, 1e-9))
                identity = false;
        }
        if (head == QLatin1String("IR") && compact != QLatin1String("IR0"))
            identity = false;
        if (head == QLatin1String("AS") && compact != QLatin1String("ASAXBY"))
            identity = false;
        if (identity)
            return true;
        return st.fail(line,
                       QStringLiteral("image transform '%1' not supported").arg(stmt));
    }
    st.warn(line, QStringLiteral("unknown extended command '%1' ignored").arg(stmt));
    return true;
}

bool processExtended(ParseState& st, const QString& content, int line)
{
    QStringList stmts = content.split(QLatin1Char('*'), Qt::SkipEmptyParts);
    for (QString& s : stmts)
        s = s.trimmed();
    stmts.removeAll(QString());
    if (stmts.isEmpty())
        return true;
    if (stmts[0].startsWith(QLatin1String("AM")))
        return parseAM(st, stmts, line);
    for (const QString& s : stmts)
        if (!processExtendedStatement(st, s, line))
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// Region contour helpers
// ---------------------------------------------------------------------------

void finalizeContour(ParseState& st, int line)
{
    if (!st.contourOpen)
        return;
    st.contourOpen = false;
    GerberContour c = std::move(st.contour);
    st.contour = GerberContour{};
    if (c.segs.empty())
        return; // empty contour (D02 then D02): drop silently
    const Vec2d last = c.segs.back().to;
    if (!nearEqual(last, c.start, 1e-9)) {
        st.warn(line, QStringLiteral("unclosed region contour auto-closed"));
        GerberContourSeg closeSeg;
        closeSeg.to = c.start;
        c.segs.push_back(closeSeg);
    }
    st.contours.push_back(std::move(c));
}

// ---------------------------------------------------------------------------
// G74 single-quadrant center resolution
// ---------------------------------------------------------------------------

// In G74 mode I/J are UNSIGNED distances; the real center is the sign
// combination that yields an arc of at most 90 degrees in the commanded
// direction with the smallest start/end radius mismatch (gerbv's algorithm).
bool resolveSingleQuadrantCenter(const Vec2d& from, const Vec2d& to, double i, double j,
                                 bool cw, Vec2d& center)
{
    const double ai = std::fabs(i), aj = std::fabs(j);
    bool found = false;
    double bestMismatch = 0.0;
    for (const double si : {1.0, -1.0}) {
        for (const double sj : {1.0, -1.0}) {
            const Vec2d c = from + Vec2d{si * ai, sj * aj};
            const double r1 = from.distanceTo(c);
            const double r2 = to.distanceTo(c);
            if (r1 <= 0.0 || r2 <= 0.0)
                continue;
            const double a0 = (from - c).angle();
            const double a1 = (to - c).angle();
            const double sweep = cw ? normalizeAngle(a0 - a1) : normalizeAngle(a1 - a0);
            if (sweep > M_PI_2 + 1e-6)
                continue;
            const double mismatch = std::fabs(r1 - r2);
            if (!found || mismatch < bestMismatch) {
                found = true;
                bestMismatch = mismatch;
                center = c;
            }
        }
    }
    return found;
}

// ---------------------------------------------------------------------------
// Operations (D01/D02/D03)
// ---------------------------------------------------------------------------

bool applyOperation(ParseState& st, int op, const Vec2d& target, bool hasIJ, double i,
                    double j, int line)
{
    st.lastOp = op;
    if (op == 2) { // move
        if (st.inRegion) {
            finalizeContour(st, line);
            st.contourStart = target;
            st.haveContourStart = true;
        }
        st.cur = target;
        return true;
    }
    if (op == 3) { // flash
        if (st.inRegion)
            return st.fail(line, QStringLiteral("D03 flash inside a G36 region"));
        if (!st.aperture)
            return st.fail(line, QStringLiteral("D03 flash with no aperture selected"));
        GerberObject o;
        o.kind = GerberObjKind::Flash;
        o.dcode = st.aperture;
        o.dark = st.dark;
        o.to = target;
        o.line = line;
        st.file.objects.push_back(std::move(o));
        st.cur = target;
        return true;
    }

    // op == 1: interpolate.
    if (st.interp == 0)
        return st.fail(line, QStringLiteral("D01 before any G01/G02/G03 mode"));
    const bool isArc = (st.interp == 2 || st.interp == 3);
    const bool cw = (st.interp == 2);
    Vec2d center;
    bool fullCircle = false;
    if (isArc) {
        if (st.quadrant == 0)
            return st.fail(line,
                           QStringLiteral("arc D01 before G74/G75 quadrant mode"));
        if (st.quadrant == 75) {
            if (!hasIJ)
                return st.fail(line, QStringLiteral("G75 arc without I/J offsets"));
            center = st.cur + Vec2d{i, j};
            fullCircle = nearEqual(st.cur, target, 1e-9);
        } else { // G74 single quadrant
            if (nearEqual(st.cur, target, 1e-9))
                return st.fail(line, QStringLiteral(
                    "G74 arc with identical endpoints is undefined (needs G75)"));
            if (!resolveSingleQuadrantCenter(st.cur, target, i, j, cw, center))
                return st.fail(line, QStringLiteral(
                    "G74 arc: no valid quadrant for I/J offsets"));
        }
    }

    if (st.inRegion) {
        if (!st.contourOpen) {
            if (!st.haveContourStart) {
                st.warn(line, QStringLiteral("region contour starts without D02"));
                st.contourStart = st.cur;
            }
            st.contour = GerberContour{};
            st.contour.start = st.contourStart;
            st.contourOpen = true;
        }
        GerberContourSeg seg;
        seg.to = target;
        seg.isArc = isArc;
        seg.center = center;
        seg.cw = cw;
        st.contour.segs.push_back(seg);
        st.cur = target;
        return true;
    }

    if (!st.aperture)
        return st.fail(line, QStringLiteral("D01 draw with no aperture selected"));
    GerberObject o;
    o.kind = isArc ? GerberObjKind::Arc : GerberObjKind::Draw;
    o.dcode = st.aperture;
    o.dark = st.dark;
    o.from = st.cur;
    o.to = target;
    o.center = center;
    o.cw = cw;
    o.fullCircle = fullCircle;
    o.line = line;
    st.file.objects.push_back(std::move(o));
    st.cur = target;
    return true;
}

bool applyGCode(ParseState& st, int g, int line)
{
    switch (g) {
    case 1: case 2: case 3:
        st.interp = g;
        return true;
    case 36:
        if (st.inRegion)
            return st.fail(line, QStringLiteral("nested G36"));
        st.inRegion = true;
        st.regionLine = line;
        st.contours.clear();
        st.contourOpen = false;
        st.haveContourStart = false;
        return true;
    case 37: {
        if (!st.inRegion)
            return st.fail(line, QStringLiteral("G37 without G36"));
        finalizeContour(st, line);
        st.inRegion = false;
        if (st.contours.empty()) {
            st.warn(line, QStringLiteral("empty G36/G37 region"));
            return true;
        }
        GerberObject o;
        o.kind = GerberObjKind::Region;
        o.dark = st.dark;
        o.contours = std::move(st.contours);
        o.line = st.regionLine;
        st.contours.clear();
        st.file.objects.push_back(std::move(o));
        return true;
    }
    case 70: case 71: {
        const GerberUnit u = (g == 70) ? GerberUnit::Inches : GerberUnit::Millimeters;
        if (st.file.unit == GerberUnit::Unknown)
            st.file.unit = u;
        else if (st.file.unit != u)
            st.warn(line, QStringLiteral("G%1 conflicts with %MO; keeping %MO").arg(g));
        return true;
    }
    case 74:
        st.quadrant = 74;
        return true;
    case 75:
        st.quadrant = 75;
        return true;
    case 90:
        return true; // absolute — the only supported mode anyway
    case 91:
        return st.fail(line, QStringLiteral("incremental coordinates (G91) not supported"));
    case 54: case 55:
        return true; // deprecated prefixes (aperture select / flash prepare)
    default:
        st.warn(line, QStringLiteral("unknown G-code G%1 ignored").arg(g));
        return true;
    }
}

bool processWord(ParseState& st, const QString& rawWord, int line)
{
    QString word = rawWord.trimmed();
    if (word.isEmpty())
        return true;

    // Comments — plus the Altium/KiCad X2-attributes-in-comments dialect.
    if (word.startsWith(QLatin1String("G04"))) {
        QString body = word.mid(3).trimmed();
        if (body.startsWith(QLatin1String("#@!"))) {
            const QString attr = body.mid(3).trimmed();
            if (attr.startsWith(QLatin1String("TF")) || attr.startsWith(QLatin1String("TA")) ||
                attr.startsWith(QLatin1String("TO")) || attr.startsWith(QLatin1String("TD")))
                recordAttribute(st, attr);
        }
        return true;
    }

    if (word == QLatin1String("M02") || word == QLatin1String("M2") ||
        word == QLatin1String("M00")) {
        if (word == QLatin1String("M00"))
            st.warn(line, QStringLiteral("M00 treated as end of file"));
        st.file.sawM02 = true;
        st.ended = true;
        return true;
    }
    if (word == QLatin1String("M01"))
        return true; // optional stop: ignore

    QString w = word;
    w.remove(QLatin1Char(' '));
    w.remove(QLatin1Char('\t'));

    // Leading G-codes (there may be several: "G54D10", "G01X..D01").
    while (w.startsWith(QLatin1Char('G'))) {
        int pos = 1;
        while (pos < w.size() && w[pos].isDigit())
            ++pos;
        if (pos == 1)
            return st.fail(line, QStringLiteral("malformed G-code in '%1'").arg(word));
        const int g = w.mid(1, pos - 1).toInt();
        if (!applyGCode(st, g, line))
            return false;
        w = w.mid(pos);
    }
    if (w.isEmpty())
        return true;

    // Coordinate data and/or D-code.
    bool hasX = false, hasY = false, hasI = false, hasJ = false;
    double x = 0, y = 0, i = 0, j = 0;
    int dnum = -1;
    int pos = 0;
    while (pos < w.size()) {
        const QChar c = w[pos];
        if (c == QLatin1Char('X') || c == QLatin1Char('Y') || c == QLatin1Char('I') ||
            c == QLatin1Char('J')) {
            ++pos;
            const int start = pos;
            if (pos < w.size() && (w[pos] == QLatin1Char('+') || w[pos] == QLatin1Char('-')))
                ++pos;
            while (pos < w.size() && (w[pos].isDigit() || w[pos] == QLatin1Char('.')))
                ++pos;
            const QString field = w.mid(start, pos - start);
            if (!st.file.format.valid)
                return st.fail(line, QStringLiteral("coordinate before %FS format statement"));
            double v = 0;
            if (!decodeCoord(field, st.file.format, v))
                return st.fail(line, QStringLiteral("bad coordinate '%1%2'").arg(c).arg(field));
            if (c == QLatin1Char('X')) { x = v; hasX = true; }
            else if (c == QLatin1Char('Y')) { y = v; hasY = true; }
            else if (c == QLatin1Char('I')) { i = v; hasI = true; }
            else { j = v; hasJ = true; }
        } else if (c == QLatin1Char('D')) {
            ++pos;
            const int start = pos;
            while (pos < w.size() && w[pos].isDigit())
                ++pos;
            if (pos == start || pos != w.size())
                return st.fail(line, QStringLiteral("malformed D-code in '%1'").arg(word));
            dnum = w.mid(start, pos - start).toInt();
        } else {
            return st.fail(line, QStringLiteral("unexpected '%1' in word '%2'").arg(c).arg(word));
        }
    }

    const bool hasCoords = hasX || hasY || hasI || hasJ;
    if (dnum >= 10) {
        if (hasCoords)
            return st.fail(line,
                           QStringLiteral("aperture select D%1 mixed with coordinates").arg(dnum));
        if (!st.file.apertures.count(dnum))
            return st.fail(line, QStringLiteral("undefined aperture D%1").arg(dnum));
        st.aperture = dnum;
        return true;
    }
    if (dnum > 3 && dnum >= 0)
        return st.fail(line, QStringLiteral("invalid operation code D%1").arg(dnum));

    int op = dnum;
    if (op < 0) {
        // Deprecated: coordinate word without operation code repeats the last one.
        if (st.lastOp == 0)
            return st.fail(line,
                           QStringLiteral("coordinate word without an operation code"));
        if (!st.warnedModalOp) {
            st.warn(line, QStringLiteral("coordinate without D-code repeats last operation "
                                         "(deprecated)"));
            st.warnedModalOp = true;
        }
        op = st.lastOp;
    }
    const Vec2d target{hasX ? x : st.cur.x, hasY ? y : st.cur.y};
    return applyOperation(st, op, target, hasI || hasJ, i, j, line);
}

// ---------------------------------------------------------------------------
// Inch -> mm pass (the ONLY place units are converted)
// ---------------------------------------------------------------------------

void scaleMacroPrim(GerberMacroPrim& p, double s)
{
    auto conv = [&](size_t idx) {
        if (idx < p.params.size())
            p.params[idx] *= s;
    };
    switch (p.code) {
    case 1:
        conv(1); conv(2); conv(3);
        break;
    case 20:
        for (size_t k = 1; k <= 5; ++k) conv(k);
        break;
    case 21:
        for (size_t k = 1; k <= 4; ++k) conv(k);
        break;
    case 4: {
        const int n = int(std::lround(p.params[1]));
        for (size_t k = 2; k < size_t(2 + 2 * (n + 1)); ++k) conv(k);
        break;
    }
    case 5:
        conv(2); conv(3); conv(4);
        break;
    case 6:
        conv(0); conv(1); conv(2); conv(3); conv(4); conv(6); conv(7);
        break;
    case 7:
        for (size_t k = 0; k <= 4; ++k) conv(k);
        break;
    case 22:
        for (size_t k = 1; k <= 4; ++k) conv(k);
        break;
    default:
        break; // unknown code: left raw (conversion stage skips it anyway)
    }
}

void convertToMm(GerberFile& f)
{
    if (f.unit != GerberUnit::Inches)
        return;
    const double s = kInchToMm;
    for (auto& [dcode, ap] : f.apertures) {
        (void)dcode;
        for (double& v : ap.params) v *= s;
        if (ap.kind == 'P' && ap.params.size() >= 3) {
            // Only the outer diameter is a length; restore count + rotation.
            ap.params[1] /= s;
            ap.params[2] /= s;
        }
        ap.holeDiameter *= s;
    }
    for (auto& [name, macro] : f.macros) {
        (void)name;
        for (GerberMacroPrim& p : macro.prims)
            scaleMacroPrim(p, s);
    }
    for (GerberObject& o : f.objects) {
        o.from = o.from * s;
        o.to = o.to * s;
        o.center = o.center * s;
        for (GerberContour& c : o.contours) {
            c.start = c.start * s;
            for (GerberContourSeg& seg : c.segs) {
                seg.to = seg.to * s;
                seg.center = seg.center * s;
            }
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// parseGerber / parseGerberData
// ---------------------------------------------------------------------------

GerberParseResult parseGerberData(const QByteArray& data)
{
    GerberParseResult res;
    ParseState st;
    int line = 1;
    int i = 0;
    const int n = data.size();
    while (i < n && !st.ended && st.error.isEmpty()) {
        const char c = data[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (c == '\r' || c == ' ' || c == '\t') { ++i; continue; }
        if (c == '%') {
            const int startLine = line;
            ++i;
            QString content;
            while (i < n && data[i] != '%') {
                if (data[i] == '\n') { ++line; }
                else if (data[i] != '\r') { content += QLatin1Char(data[i]); }
                ++i;
            }
            if (i >= n) {
                st.fail(startLine, QStringLiteral("unterminated extended command"));
                break;
            }
            ++i; // closing '%'
            if (!processExtended(st, content, startLine))
                break;
        } else {
            const int startLine = line;
            QString word;
            while (i < n && data[i] != '*') {
                if (data[i] == '\n') { ++line; }
                else if (data[i] != '\r') { word += QLatin1Char(data[i]); }
                ++i;
            }
            if (i >= n) {
                if (!word.trimmed().isEmpty())
                    st.fail(startLine, QStringLiteral("word without terminating '*'"));
                break;
            }
            ++i; // '*'
            if (!processWord(st, word, startLine))
                break;
        }
    }

    if (st.error.isEmpty()) {
        if (st.inRegion)
            st.fail(st.regionLine, QStringLiteral("G36 region never closed by G37"));
        else if (!st.file.sawM02)
            st.warn(line, QStringLiteral("missing M02 end-of-file"));
    }
    if (st.error.isEmpty() && st.file.unit == GerberUnit::Unknown &&
        (!st.file.objects.empty() || !st.file.apertures.empty()))
        st.fail(1, QStringLiteral("no unit declaration (%MO or G70/G71)"));
    if (st.error.isEmpty()) {
        for (const auto& [dcode, ap] : st.file.apertures) {
            if (ap.kind == 'M' && !st.file.macros.count(ap.macroName)) {
                st.fail(1, QStringLiteral("aperture D%1 references undefined macro '%2'")
                               .arg(dcode).arg(ap.macroName));
                break;
            }
        }
    }

    if (!st.error.isEmpty()) {
        res.error = st.error;
        return res;
    }
    convertToMm(st.file);
    res.file = std::move(st.file);
    res.ok = true;
    return res;
}

GerberParseResult parseGerber(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        GerberParseResult res;
        res.error = QStringLiteral("cannot open '%1'").arg(path);
        return res;
    }
    return parseGerberData(f.readAll());
}

// ---------------------------------------------------------------------------
// Stage 2 — gerberToDocument
// ---------------------------------------------------------------------------

namespace {

Vec2d rotDeg(const Vec2d& p, double degrees)
{
    return nearZero(degrees, 1e-12) ? p : p.rotated(degrees * M_PI / 180.0);
}

std::vector<Vec2d> circleRing(const Vec2d& center, double radius)
{
    std::vector<Vec2d> ring;
    flattenArc(center, radius, 0.0, 2.0 * M_PI, kRingTol, ring);
    if (!ring.empty())
        ring.pop_back(); // drop the duplicated closing point
    return ring;
}

// Solid contours of one aperture, centered on the flash origin. mm.
std::vector<std::vector<Vec2d>> apertureRings(const GerberAperture& ap,
                                              const GerberFile& file,
                                              QStringList& warnings)
{
    std::vector<std::vector<Vec2d>> rings;
    switch (ap.kind) {
    case 'C': {
        const double r = ap.params[0] * 0.5;
        if (r > 0)
            rings.push_back(circleRing({0, 0}, r));
        break;
    }
    case 'R': {
        const double hw = ap.params[0] * 0.5, hh = ap.params[1] * 0.5;
        rings.push_back({{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}});
        break;
    }
    case 'O': {
        const double w = ap.params[0], h = ap.params[1];
        if (nearEqual(w, h, 1e-12)) {
            rings.push_back(circleRing({0, 0}, w * 0.5));
            break;
        }
        std::vector<Vec2d> ring;
        if (w > h) { // horizontal stadium: caps at x = +-(w-h)/2, radius h/2
            const double cx = (w - h) * 0.5, r = h * 0.5;
            flattenArc({cx, 0}, r, -M_PI_2, M_PI, kRingTol, ring);
            flattenArc({-cx, 0}, r, M_PI_2, M_PI, kRingTol, ring);
        } else { // vertical stadium
            const double cy = (h - w) * 0.5, r = w * 0.5;
            flattenArc({0, cy}, r, 0.0, M_PI, kRingTol, ring);
            flattenArc({0, -cy}, r, M_PI, M_PI, kRingTol, ring);
        }
        rings.push_back(std::move(ring));
        break;
    }
    case 'P': {
        const double r = ap.params[0] * 0.5;
        const int nv = int(std::lround(ap.params[1]));
        const double rot = ap.params[2] * M_PI / 180.0;
        std::vector<Vec2d> ring;
        for (int k = 0; k < nv; ++k)
            ring.push_back(Vec2d::polar(r, rot + 2.0 * M_PI * k / nv));
        rings.push_back(std::move(ring));
        break;
    }
    case 'M': {
        const auto it = file.macros.find(ap.macroName);
        if (it == file.macros.end())
            break; // parser validated; defensive
        for (const GerberMacroPrim& p : it->second.prims) {
            if (!p.params.empty() && nearZero(p.params[0], 1e-9) &&
                (p.code == 1 || p.code == 4 || p.code == 5 || p.code == 20 ||
                 p.code == 21 || p.code == 22)) {
                warnings << QStringLiteral(
                    "macro '%1': exposure-off primitive skipped (not rendered)")
                                .arg(ap.macroName);
                continue;
            }
            std::vector<Vec2d> ring;
            double rot = 0.0;
            switch (p.code) {
            case 1: {
                const double r = p.params[1] * 0.5;
                const Vec2d c{p.params[2], p.params[3]};
                rot = p.params.size() > 4 ? p.params[4] : 0.0;
                if (r > 0)
                    ring = circleRing(c, r);
                break;
            }
            case 20: {
                const double hw = p.params[1] * 0.5;
                const Vec2d s{p.params[2], p.params[3]}, e{p.params[4], p.params[5]};
                rot = p.params[6];
                if (nearEqual(s, e, 1e-12)) {
                    warnings << QStringLiteral("macro '%1': zero-length vector line skipped")
                                    .arg(ap.macroName);
                    break;
                }
                const Vec2d o = (e - s).normalized().perp() * hw;
                ring = {s + o, e + o, e - o, s - o}; // flat (rectangular) ends
                break;
            }
            case 21: {
                const double hw = p.params[1] * 0.5, hh = p.params[2] * 0.5;
                const Vec2d c{p.params[3], p.params[4]};
                rot = p.params[5];
                ring = {c + Vec2d{-hw, -hh}, c + Vec2d{hw, -hh}, c + Vec2d{hw, hh},
                        c + Vec2d{-hw, hh}};
                break;
            }
            case 22: { // lower-left rectangle
                const double w = p.params[1], h = p.params[2];
                const Vec2d ll{p.params[3], p.params[4]};
                rot = p.params.size() > 5 ? p.params[5] : 0.0;
                ring = {ll, ll + Vec2d{w, 0}, ll + Vec2d{w, h}, ll + Vec2d{0, h}};
                break;
            }
            case 4: {
                const int nv = int(std::lround(p.params[1]));
                for (int k = 0; k <= nv; ++k)
                    ring.push_back({p.params[size_t(2 + 2 * k)],
                                    p.params[size_t(3 + 2 * k)]});
                rot = p.params[size_t(2 + 2 * (nv + 1))];
                if (ring.size() > 1 && nearEqual(ring.front(), ring.back(), 1e-9))
                    ring.pop_back();
                break;
            }
            case 5: {
                const int nv = int(std::lround(p.params[1]));
                const Vec2d c{p.params[2], p.params[3]};
                const double r = p.params[4] * 0.5;
                rot = p.params[5];
                for (int k = 0; k < nv; ++k)
                    ring.push_back(c + Vec2d::polar(r, 2.0 * M_PI * k / nv));
                break;
            }
            default:
                warnings << QStringLiteral(
                    "macro '%1': primitive code %2 not rendered")
                                .arg(ap.macroName).arg(p.code);
                break;
            }
            if (ring.size() >= 3) {
                // X2 spec: rotation is around the MACRO origin, degrees CCW.
                for (Vec2d& pt : ring)
                    pt = rotDeg(pt, rot);
                rings.push_back(std::move(ring));
            }
        }
        break;
    }
    default:
        break;
    }
    if (ap.holeDiameter > 0)
        warnings << QStringLiteral(
            "aperture D%1: round hole kept in table but rendered filled")
                        .arg(ap.dcode);
    return rings;
}

// Stroke width for a draw/arc with this aperture. Only round apertures are
// exact; others are approximated (round pen) with a warning by the caller.
double apertureStrokeWidth(const GerberAperture& ap, const GerberFile& file,
                           bool& exact)
{
    exact = (ap.kind == 'C');
    switch (ap.kind) {
    case 'C':
    case 'P':
        return ap.params[0];
    case 'R':
    case 'O':
        return std::min(ap.params[0], ap.params[1]);
    case 'M': {
        // Bounding-box smaller side of the rendered macro.
        QStringList scratch;
        BBox2d box;
        for (const auto& ring : apertureRings(ap, file, scratch))
            for (const Vec2d& p : ring)
                box.expand(p);
        if (!box.isValid())
            return 0.0;
        return std::min(box.width(), box.height());
    }
    default:
        return 0.0;
    }
}

// Signed sweep of an arc (radians): >0 CCW, <0 CW. Full circles are handled
// by the caller (from == to normalizes to 0 here).
double arcSweep(const Vec2d& from, const Vec2d& to, const Vec2d& center, bool cw)
{
    const double a0 = (from - center).angle();
    const double a1 = (to - center).angle();
    return cw ? -normalizeAngle(a0 - a1) : normalizeAngle(a1 - a0);
}

} // namespace

GerberImportResult gerberToDocument(Document& doc, const GerberFile& file,
                                    const QString& layerName)
{
    GerberImportResult res;
    if (layerName.isEmpty() || layerName.contains(QLatin1Char(' ')) ||
        layerName.contains(QLatin1Char('\t'))) {
        res.error = QStringLiteral("layer name must be a single non-empty token");
        return res;
    }
    res.warnings = file.warnings;

    const LayerId layerId = doc.ensureLayer(layerName);
    TransactionScope tx(doc, QStringLiteral("GERBERIMPORT"));

    // One block definition per flashed aperture.
    std::map<int, QString> blockName;
    std::map<int, bool> widthWarned;

    auto blockFor = [&](int dcode) -> QString {
        auto it = blockName.find(dcode);
        if (it != blockName.end())
            return it->second;
        QString name = QStringLiteral("GBR-D%1").arg(dcode);
        for (int suffix = 2; doc.blockByName(name); ++suffix)
            name = QStringLiteral("GBR-D%1-%2").arg(dcode).arg(suffix);
        BlockDef* def = doc.createBlock(name, {0, 0});
        const GerberAperture& ap = file.apertures.at(dcode);
        auto rings = apertureRings(ap, file, res.warnings);
        if (!rings.empty()) {
            auto hatch = std::make_unique<HatchEntity>();
            hatch->rings = std::move(rings);
            hatch->pattern = QStringLiteral("SOLID");
            hatch->setLayerId(layerId);
            def->entities.push_back(std::move(hatch));
        } else {
            res.warnings << QStringLiteral("aperture D%1 produced no filled contour")
                                .arg(dcode);
        }
        ++res.blocks;
        blockName[dcode] = name;
        return name;
    };

    // Pending polyline: consecutive continuous strokes of one aperture+polarity.
    struct Pending {
        bool active = false;
        int dcode = 0;
        bool dark = true;
        double width = 0.0;
        std::vector<PolyVertex> verts;
    } pending;

    auto addEntity = [&](std::unique_ptr<Entity> e, bool dark) {
        e->setLayerId(layerId);
        if (!dark)
            e->setExtraValue(QStringLiteral("gpol"), QStringLiteral("C"));
        doc.addEntity(std::move(e));
        ++res.entities;
    };

    auto flush = [&]() {
        if (!pending.active)
            return;
        if (pending.verts.size() >= 2) {
            auto pl = std::make_unique<PolylineEntity>(pending.verts, false);
            pl->setWidth(pending.width);
            addEntity(std::move(pl), pending.dark);
        }
        pending.active = false;
        pending.verts.clear();
    };

    for (const GerberObject& o : file.objects) {
        switch (o.kind) {
        case GerberObjKind::Draw:
        case GerberObjKind::Arc: {
            const auto apIt = file.apertures.find(o.dcode);
            if (apIt == file.apertures.end()) {
                res.error = QStringLiteral("line %1: object uses unknown aperture D%2")
                                .arg(o.line).arg(o.dcode);
                return res;
            }
            bool exact = true;
            const double width = apertureStrokeWidth(apIt->second, file, exact);
            if (!exact && !widthWarned[o.dcode]) {
                widthWarned[o.dcode] = true;
                res.warnings << QStringLiteral(
                    "line %1: draw with non-round aperture D%2 ('%3') approximated by a "
                    "round stroke %4 mm wide")
                                    .arg(o.line).arg(o.dcode)
                                    .arg(QLatin1Char(apIt->second.kind))
                                    .arg(width);
            }
            const bool continues = pending.active && pending.dcode == o.dcode &&
                                   pending.dark == o.dark &&
                                   nearEqual(pending.verts.back().pos, o.from, 1e-9) &&
                                   nearEqual(pending.width, width, 1e-9);
            if (!continues) {
                flush();
                pending.active = true;
                pending.dcode = o.dcode;
                pending.dark = o.dark;
                pending.width = width;
                pending.verts.push_back({o.from, 0.0});
            }
            if (o.kind == GerberObjKind::Draw) {
                pending.verts.push_back({o.to, 0.0});
                ++res.draws;
            } else if (o.fullCircle) {
                // 360-degree arc: two half circles (|bulge| = tan(90/2) = 1).
                const double b = o.cw ? -1.0 : 1.0;
                const Vec2d opposite = o.center * 2.0 - o.from;
                pending.verts.back().bulge = b;
                pending.verts.push_back({opposite, b});
                pending.verts.push_back({o.to, 0.0});
                ++res.arcs;
            } else {
                const double sweep = arcSweep(o.from, o.to, o.center, o.cw);
                pending.verts.back().bulge = std::tan(sweep / 4.0);
                pending.verts.push_back({o.to, 0.0});
                ++res.arcs;
            }
            break;
        }
        case GerberObjKind::Flash: {
            flush();
            if (!file.apertures.count(o.dcode)) {
                res.error = QStringLiteral("line %1: flash uses unknown aperture D%2")
                                .arg(o.line).arg(o.dcode);
                return res;
            }
            auto ins = std::make_unique<InsertEntity>();
            ins->blockName = blockFor(o.dcode);
            ins->position = o.to;
            addEntity(std::move(ins), o.dark);
            ++res.flashes;
            break;
        }
        case GerberObjKind::Region: {
            flush();
            auto hatch = std::make_unique<HatchEntity>();
            hatch->pattern = QStringLiteral("SOLID");
            for (const GerberContour& c : o.contours) {
                std::vector<Vec2d> ring;
                ring.push_back(c.start);
                for (const GerberContourSeg& seg : c.segs) {
                    const Vec2d prev = ring.back();
                    if (!seg.isArc) {
                        ring.push_back(seg.to);
                        continue;
                    }
                    const double radius = prev.distanceTo(seg.center);
                    const double a0 = (prev - seg.center).angle();
                    double sweep = arcSweep(prev, seg.to, seg.center, seg.cw);
                    if (nearEqual(prev, seg.to, 1e-9))
                        sweep = seg.cw ? -2.0 * M_PI : 2.0 * M_PI; // full-circle cut-in
                    std::vector<Vec2d> pts;
                    flattenArc(seg.center, radius, a0, sweep, kRingTol, pts);
                    ring.insert(ring.end(), pts.begin() + 1, pts.end());
                }
                if (ring.size() > 1 && nearEqual(ring.front(), ring.back(), 1e-9))
                    ring.pop_back();
                if (ring.size() >= 3)
                    hatch->rings.push_back(std::move(ring));
                else
                    res.warnings << QStringLiteral(
                        "line %1: degenerate region contour dropped").arg(o.line);
            }
            if (hatch->rings.empty()) {
                res.warnings << QStringLiteral("line %1: region without usable contours")
                                    .arg(o.line);
            } else {
                addEntity(std::move(hatch), o.dark);
                ++res.regions;
            }
            break;
        }
        }
    }
    flush();

    tx.commit();
    res.ok = true;
    return res;
}

} // namespace viki
