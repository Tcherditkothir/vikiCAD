#include "EagleImporter.h"

#include <cctype>
#include <cmath>
#include <algorithm>

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QXmlStreamReader>

#include "doc/Annotations.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"

// EAGLE 6+ XML importer, viewing-grade. Everything in an EAGLE file is
// already millimeters, so no unit conversion happens anywhere. The drawing
// is flattened: package/symbol primitives are transformed to their element/
// instance placement and added as plain document entities — no blocks, so
// bottom-side mirroring and per-instance >NAME/>VALUE substitution need no
// instance machinery. Non-graphic data (contactrefs, classes, design rules,
// autorouter setups) is intentionally not represented.

namespace viki {
namespace {

constexpr double kChordTol = 0.02;  // mm, pad/via ring tessellation
constexpr double kSheetGap = 25.0;  // mm between side-by-side sheets

// ---------------------------------------------------------------------------
// Generic XML tree (QXmlStreamReader is QtCore; QtXml stays unlinked)
// ---------------------------------------------------------------------------

struct ENode {
    QString name;
    QHash<QString, QString> attrs;
    std::vector<ENode> children;
    QString text;

    QString attr(const char* k, const QString& def = QString()) const
    {
        const auto it = attrs.constFind(QLatin1String(k));
        return it == attrs.constEnd() ? def : *it;
    }
    double num(const char* k, double def = 0.0) const
    {
        const auto it = attrs.constFind(QLatin1String(k));
        if (it == attrs.constEnd())
            return def;
        bool ok = false;
        const double v = it->toDouble(&ok);
        return ok ? v : def;
    }
    bool has(const char* k) const { return attrs.contains(QLatin1String(k)); }
    const ENode* child(const char* n) const
    {
        for (const auto& c : children)
            if (c.name == QLatin1String(n))
                return &c;
        return nullptr;
    }
};

// Reads the element the reader is positioned on (StartElement) into `out`.
void readNode(QXmlStreamReader& xml, ENode& out)
{
    out.name = xml.name().toString();
    for (const auto& a : xml.attributes())
        out.attrs.insert(a.name().toString(), a.value().toString());
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::StartElement:
            out.children.emplace_back();
            readNode(xml, out.children.back());
            break;
        case QXmlStreamReader::Characters:
            if (!xml.isWhitespace())
                out.text += xml.text();
            break;
        case QXmlStreamReader::EndElement:
            return;
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// EAGLE fixed bits: palette, rotation grammar, placement transform
// ---------------------------------------------------------------------------

// The classic EAGLE 16-color palette (black background). Files reference
// colors by index in the layer table; indices past 15 wrap.
const uint32_t kPalette[16] = {
    0x000000, 0x0000B4, 0x00B400, 0x00B4B4, 0xB40000, 0xB400B4, 0xB4B400,
    0xB4B4B4, 0x646464, 0x6464FF, 0x64FF64, 0x64FFFF, 0xFF6464, 0xFF64FF,
    0xFFFF64, 0xFFFFFF,
};

// EAGLE rotation attribute: [S][M]R<angle>, e.g. "R90", "MR180", "SR270".
struct ERot {
    double deg = 0.0;
    bool mirror = false;
    bool spin = false;
};

ERot parseRot(const QString& s)
{
    ERot r;
    int i = 0;
    for (; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('S'))
            r.spin = true;
        else if (c == QLatin1Char('M'))
            r.mirror = true;
        else if (c == QLatin1Char('R'))
            break;
    }
    if (i < s.size())
        r.deg = QStringView(s).mid(i + 1).toDouble();
    return r;
}

// Element/instance placement: mirror across Y first, then rotate, then move.
struct Placement {
    Vec2d pos;
    double deg = 0.0;
    bool mirror = false;

    Vec2d apply(const Vec2d& local) const
    {
        Vec2d p = local;
        if (mirror)
            p.x = -p.x;
        const double a = deg * M_PI / 180.0;
        const double ca = std::cos(a), sa = std::sin(a);
        return {pos.x + p.x * ca - p.y * sa, pos.y + p.x * sa + p.y * ca};
    }
};

double bulgeFromCurve(double curveDeg, bool mirrored)
{
    if (curveDeg == 0.0)
        return 0.0;
    const double b = std::tan(curveDeg * M_PI / 180.0 / 4.0);
    return mirrored ? -b : b;
}

// ---------------------------------------------------------------------------
// Text helpers: alignment grammar + EAGLE's read-up rule
// ---------------------------------------------------------------------------

struct TextAlign {
    TextHAlign h = TextHAlign::Left;
    TextVAlign v = TextVAlign::Bottom;
};

TextAlign parseAlign(const QString& s)
{
    TextAlign a;
    if (s.isEmpty())
        return a; // bottom-left, the EAGLE default
    if (s.startsWith(QLatin1String("top")))
        a.v = TextVAlign::Top;
    else if (s.startsWith(QLatin1String("center")))
        a.v = TextVAlign::Middle;
    if (s.endsWith(QLatin1String("right")))
        a.h = TextHAlign::Right;
    else if (s.endsWith(QLatin1String("center")) || s == QLatin1String("center"))
        a.h = TextHAlign::Center;
    return a;
}

TextHAlign flipH(TextHAlign h)
{
    if (h == TextHAlign::Left)
        return TextHAlign::Right;
    if (h == TextHAlign::Right)
        return TextHAlign::Left;
    return h;
}

TextVAlign flipV(TextVAlign v)
{
    if (v == TextVAlign::Bottom)
        return TextVAlign::Top;
    if (v == TextVAlign::Top)
        return TextVAlign::Bottom;
    return v;
}

// ---------------------------------------------------------------------------
// Importer
// ---------------------------------------------------------------------------

struct FileLayer {
    QString name;
    uint32_t rgb = 0xB4B4B4;
    bool visible = true;
};

// Values substituted into ">KEY" texts. Board and schematic fill what they
// know; an unknown key keeps the literal text (that is what EAGLE prints
// for a key it cannot resolve either).
struct Subst {
    QString name;
    QString value;
    QString gate;
    QString part;
    QString sheet;        // "n/total"
    QString drawingName;
};

class Importer {
public:
    explicit Importer(EagleImportResult& result)
        : m_result(result)
    {
        m_result.document = std::make_unique<Document>();
        m_doc = m_result.document.get();
    }

    enum class Kind { Board, Schematic, Library };
    void run(const ENode& drawing, const ENode& section, Kind kind);

private:
    EagleImportResult& m_result;
    Document* m_doc = nullptr;

    QHash<int, FileLayer> m_layerTable;
    QHash<int, LayerId> m_layerIds;
    LayerId m_anyLayer = 0;

    std::vector<std::unique_ptr<Entity>> m_staged;

    // ---- bookkeeping ----

    void skip(const QString& type)
    {
        ++m_result.skipped;
        if (!m_result.skippedTypes.contains(type))
            m_result.skippedTypes.append(type);
    }

    // EAGLE paints boards bottom-copper first and annotations last; ranks
    // reproduce that stacking (lower rank = painted first).
    static int paintRank(int n)
    {
        if (n >= 39 && n <= 43)
            return 4;   // keepout/restrict
        if (n >= 29 && n <= 38)
            return 6;   // stop/cream/finish/glue/test
        if (n == 97 || n == 98)
            return 8;   // info/guide
        if (n == 16)
            return 10;  // Bottom
        if (n >= 2 && n <= 15)
            return 12;  // inner copper
        if (n == 1)
            return 14;  // Top
        if (n == 94 || n == 93)
            return 15;  // symbols/pins
        if (n == 91 || n == 92)
            return 16;  // nets/busses
        if (n == 17 || n == 18)
            return 20;  // pads/vias
        if (n == 44 || n == 45 || n == 46)
            return 24;  // drills/holes/milling
        if (n == 21 || n == 22 || n == 51 || n == 52)
            return 26;  // silk + docu
        if (n >= 25 && n <= 28)
            return 28;  // names/values
        if (n == 95 || n == 96)
            return 28;
        if (n == 20)
            return 30;  // dimension
        return 18;
    }

    LayerId layerFor(int n)
    {
        auto it = m_layerIds.constFind(n);
        if (it != m_layerIds.constEnd())
            return *it;
        FileLayer fl = m_layerTable.value(
            n, FileLayer{QStringLiteral("L%1").arg(n), 0xB4B4B4, true});
        const LayerId id = m_doc->ensureLayer(fl.name, fl.rgb, fl.visible);
        m_doc->setLayerRank(id, paintRank(n));
        m_layerIds.insert(n, id);
        if (m_anyLayer == 0)
            m_anyLayer = id;
        return id;
    }

    // Mirrored elements land on the paired bottom/top layer.
    static int mirrorLayer(int n)
    {
        switch (n) {
        case 1: return 16;
        case 16: return 1;
        default: break;
        }
        // t/b pairs: 21..42 odd/even neighbours, 51/52.
        if ((n >= 21 && n <= 42) || n == 51)
            return (n % 2 == 1) ? n + 1 : n - 1;
        if (n == 52)
            return 51;
        return n;
    }

    int placedLayer(int n, const Placement& pl) const
    {
        return pl.mirror ? mirrorLayer(n) : n;
    }

    void stage(std::unique_ptr<Entity> e, int eagleLayer)
    {
        e->setLayerId(layerFor(eagleLayer));
        m_staged.push_back(std::move(e));
        ++m_result.imported;
    }

    // Adds everything staged so far to the document, shifted by `offset`.
    void flushStaged(const Vec2d& offset)
    {
        for (auto& e : m_staged) {
            if (offset.x != 0.0 || offset.y != 0.0)
                e->transform(Xform2d::translation(offset));
            m_doc->restoreEntity(std::move(e), m_doc->nextId());
            m_doc->setNextId(m_doc->nextId() + 1);
        }
        m_staged.clear();
    }

    BBox2d stagedBounds() const
    {
        BBox2d box;
        for (const auto& e : m_staged)
            box.expand(e->bounds());
        return box;
    }

    // ---- drawing primitives (shared by board and schematic) ----

    void emitWire(const ENode& n, const Placement& pl);
    void emitRectangle(const ENode& n, const Placement& pl);
    void emitCircle(const ENode& n, const Placement& pl);
    void emitPolygon(const ENode& n, const Placement& pl);
    void emitText(const ENode& n, const Placement& pl, const Subst& subst);
    void emitTextAt(const QString& content, const Vec2d& localPos, double size,
                    int layer, const ERot& local, const TextAlign& align,
                    const Placement& pl);
    void emitFrame(const ENode& n, const Placement& pl);
    void emitDimension(const ENode& n, const Placement& pl);
    void emitHole(const ENode& n, const Placement& pl);
    void emitPad(const ENode& n, const Placement& pl);
    void emitSmd(const ENode& n, const Placement& pl);
    void emitVia(const ENode& n, const Placement& pl);
    void emitPin(const ENode& n, const Placement& pl);

    void emitNode(const ENode& n, const Placement& pl, const Subst& subst,
                  bool isBoard);

    QString substitute(const QString& raw, const Subst& subst) const;

    void solidRing(std::vector<Vec2d> ring, int layer);
    void annulus(const Vec2d& localCenter, std::vector<Vec2d> outerLocal,
                 double drill, int layer, const Placement& pl);
    static std::vector<Vec2d> shapeRing(const QString& shape, double outer,
                                        double rotDeg);

    // ---- board / schematic / library ----

    void runBoard(const ENode& board);
    void runSchematic(const ENode& schematic);
    void runLibrary(const ENode& library);
};

// ---------------------------------------------------------------------------

QString Importer::substitute(const QString& raw, const Subst& subst) const
{
    const QString t = raw.trimmed();
    if (!t.startsWith(QLatin1Char('>')))
        return raw;
    const QString key = t.mid(1).trimmed().toUpper();
    if (key == QLatin1String("NAME"))
        return subst.name;
    if (key == QLatin1String("VALUE"))
        return subst.value;
    if (key == QLatin1String("PART"))
        return subst.part;
    if (key == QLatin1String("GATE"))
        return subst.gate;
    if (key == QLatin1String("SHEET"))
        return subst.sheet;
    if (key == QLatin1String("DRAWING_NAME"))
        return subst.drawingName;
    if (key.startsWith(QLatin1String("LAST_DATE_TIME")) ||
        key.startsWith(QLatin1String("PLOT_DATE_TIME")) ||
        key.startsWith(QLatin1String("ASSEMBLY_VARIANT")))
        return QString();
    return raw; // unknown key: EAGLE prints it literally too
}

void Importer::emitWire(const ENode& n, const Placement& pl)
{
    const Vec2d p1 = pl.apply({n.num("x1"), n.num("y1")});
    const Vec2d p2 = pl.apply({n.num("x2"), n.num("y2")});
    std::vector<PolyVertex> vs(2);
    vs[0].pos = p1;
    vs[0].bulge = bulgeFromCurve(n.num("curve"), pl.mirror);
    vs[1].pos = p2;
    auto poly = std::make_unique<PolylineEntity>(std::move(vs), false);
    poly->setWidth(n.num("width"));
    stage(std::move(poly), placedLayer(int(n.num("layer", 94)), pl));
}

void Importer::emitRectangle(const ENode& n, const Placement& pl)
{
    const double x1 = n.num("x1"), y1 = n.num("y1");
    const double x2 = n.num("x2"), y2 = n.num("y2");
    const Vec2d c{(x1 + x2) / 2.0, (y1 + y2) / 2.0};
    const double rot = parseRot(n.attr("rot")).deg * M_PI / 180.0;
    const double ca = std::cos(rot), sa = std::sin(rot);
    std::vector<Vec2d> ring;
    for (const Vec2d& corner : {Vec2d{x1, y1}, Vec2d{x2, y1}, Vec2d{x2, y2}, Vec2d{x1, y2}}) {
        const Vec2d d = corner - c;
        ring.push_back(pl.apply({c.x + d.x * ca - d.y * sa, c.y + d.x * sa + d.y * ca}));
    }
    solidRing(std::move(ring), placedLayer(int(n.num("layer", 94)), pl));
}

void Importer::emitCircle(const ENode& n, const Placement& pl)
{
    const Vec2d center = pl.apply({n.num("x"), n.num("y")});
    const double r = n.num("radius");
    const double width = n.num("width");
    const int layer = placedLayer(int(n.num("layer", 94)), pl);
    if (r <= 0)
        return;
    if (width <= 0.0) {
        // EAGLE: zero width = filled disc.
        std::vector<Vec2d> ring;
        flattenArc(center, r, 0.0, 2.0 * M_PI, kChordTol, ring);
        solidRing(std::move(ring), layer);
        return;
    }
    // Wide outline: closed 2-vertex polyline of semicircle arcs.
    std::vector<PolyVertex> vs(2);
    vs[0].pos = {center.x - r, center.y};
    vs[0].bulge = 1.0;
    vs[1].pos = {center.x + r, center.y};
    vs[1].bulge = 1.0;
    auto poly = std::make_unique<PolylineEntity>(std::move(vs), true);
    poly->setWidth(width);
    stage(std::move(poly), layer);
}

void Importer::emitPolygon(const ENode& n, const Placement& pl)
{
    // Copper pours are stored as their outline (the pour itself is computed
    // by EAGLE at ratsnest time); the outline is the honest thing to draw.
    std::vector<PolyVertex> vs;
    for (const auto& c : n.children) {
        if (c.name != QLatin1String("vertex"))
            continue;
        PolyVertex v;
        v.pos = pl.apply({c.num("x"), c.num("y")});
        v.bulge = bulgeFromCurve(c.num("curve"), pl.mirror);
        vs.push_back(v);
    }
    if (vs.size() < 2)
        return;
    auto poly = std::make_unique<PolylineEntity>(std::move(vs), true);
    poly->setWidth(n.num("width"));
    stage(std::move(poly), placedLayer(int(n.num("layer", 94)), pl));
}

void Importer::emitTextAt(const QString& content, const Vec2d& localPos,
                          double size, int layer, const ERot& local,
                          const TextAlign& align, const Placement& pl)
{
    if (content.isEmpty())
        return;
    const Vec2d pos = pl.apply(localPos);
    double deg = pl.mirror ? pl.deg + 180.0 - local.deg : pl.deg + local.deg;
    deg = std::fmod(deg, 360.0);
    if (deg < 0)
        deg += 360.0;
    TextHAlign h = align.h;
    TextVAlign v = align.v;
    if (pl.mirror != local.mirror)
        h = flipH(h);
    // EAGLE keeps text readable: anything pointing into 90..270 flips over.
    if (!local.spin && deg > 90.0 + 1e-9 && deg <= 270.0 + 1e-9) {
        deg -= 180.0;
        h = flipH(h);
        v = flipV(v);
    }
    auto text = std::make_unique<TextEntity>(pos, size, deg * M_PI / 180.0,
                                             content);
    text->hAlign = h;
    text->vAlign = v;
    stage(std::move(text), layer);
}

void Importer::emitText(const ENode& n, const Placement& pl, const Subst& subst)
{
    const QString content = substitute(n.text, subst);
    if (content.trimmed().isEmpty())
        return;
    const ERot local = parseRot(n.attr("rot"));
    emitTextAt(content, {n.num("x"), n.num("y")}, n.num("size", 1.778),
               placedLayer(int(n.num("layer", 94)), pl), local,
               parseAlign(n.attr("align")), pl);
}

void Importer::emitFrame(const ENode& n, const Placement& pl)
{
    const double x1 = std::min(n.num("x1"), n.num("x2"));
    const double x2 = std::max(n.num("x1"), n.num("x2"));
    const double y1 = std::min(n.num("y1"), n.num("y2"));
    const double y2 = std::max(n.num("y1"), n.num("y2"));
    const int cols = std::max(1, int(n.num("columns", 8)));
    const int rows = std::max(1, int(n.num("rows", 5)));
    const int layer = placedLayer(int(n.num("layer", 94)), pl);
    const double inset = 3.81;
    const bool bl = n.attr("border-left", QStringLiteral("yes")) != QLatin1String("no");
    const bool br = n.attr("border-right", QStringLiteral("yes")) != QLatin1String("no");
    const bool bt = n.attr("border-top", QStringLiteral("yes")) != QLatin1String("no");
    const bool bb = n.attr("border-bottom", QStringLiteral("yes")) != QLatin1String("no");

    auto rect = [&](double ax, double ay, double bx, double by) {
        std::vector<PolyVertex> vs(4);
        vs[0].pos = pl.apply({ax, ay});
        vs[1].pos = pl.apply({bx, ay});
        vs[2].pos = pl.apply({bx, by});
        vs[3].pos = pl.apply({ax, by});
        stage(std::make_unique<PolylineEntity>(std::move(vs), true), layer);
    };
    auto seg = [&](double ax, double ay, double bx, double by) {
        std::vector<PolyVertex> vs(2);
        vs[0].pos = pl.apply({ax, ay});
        vs[1].pos = pl.apply({bx, by});
        stage(std::make_unique<PolylineEntity>(std::move(vs), false), layer);
    };
    auto label = [&](const QString& s, double cx, double cy) {
        TextAlign a;
        a.h = TextHAlign::Center;
        a.v = TextVAlign::Middle;
        emitTextAt(s, {cx, cy}, 2.54, layer, ERot{}, a, pl);
    };

    rect(x1, y1, x2, y2);
    const double ix1 = bl ? x1 + inset : x1;
    const double ix2 = br ? x2 - inset : x2;
    const double iy1 = bb ? y1 + inset : y1;
    const double iy2 = bt ? y2 - inset : y2;
    rect(ix1, iy1, ix2, iy2);
    for (int c = 1; c < cols; ++c) {
        const double x = ix1 + (ix2 - ix1) * c / cols;
        if (bb)
            seg(x, y1, x, iy1);
        if (bt)
            seg(x, iy2, x, y2);
    }
    for (int c = 0; c < cols; ++c) {
        const double cx = ix1 + (ix2 - ix1) * (c + 0.5) / cols;
        const QString s = QString(QChar(QLatin1Char('A').unicode() + c % 26));
        if (bb)
            label(s, cx, (y1 + iy1) / 2.0);
        if (bt)
            label(s, cx, (iy2 + y2) / 2.0);
    }
    for (int r = 1; r < rows; ++r) {
        const double y = iy1 + (iy2 - iy1) * r / rows;
        if (bl)
            seg(x1, y, ix1, y);
        if (br)
            seg(ix2, y, x2, y);
    }
    for (int r = 0; r < rows; ++r) {
        const double cy = iy1 + (iy2 - iy1) * (r + 0.5) / rows;
        const QString s = QString::number(rows - r);
        if (bl)
            label(s, (x1 + ix1) / 2.0, cy);
        if (br)
            label(s, (ix2 + x2) / 2.0, cy);
    }
}

void Importer::emitDimension(const ENode& n, const Placement& pl)
{
    const Vec2d p1 = pl.apply({n.num("x1"), n.num("y1")});
    const Vec2d p2 = pl.apply({n.num("x2"), n.num("y2")});
    const Vec2d p3 = pl.apply({n.num("x3"), n.num("y3")});
    const QString dtype = n.attr("dtype", QStringLiteral("parallel"));
    const int layer = placedLayer(int(n.num("layer", 20)), pl);
    // EAGLE dimension text is absolute; ours comes from the style (3.5 mm
    // in "Standard"), so the per-entity scale bridges the two.
    const double scale = n.num("textsize", 2.54) / 3.5;

    if (dtype == QLatin1String("leader")) {
        auto leader = std::make_unique<LeaderEntity>();
        leader->points = {p1, p2, p3};
        leader->styleScale = scale;
        stage(std::move(leader), layer);
        return;
    }
    auto dim = std::make_unique<DimensionEntity>();
    dim->a = p1;
    dim->b = p2;
    dim->pos = p3;
    dim->styleScale = scale;
    if (dtype == QLatin1String("horizontal")) {
        dim->kind = DimensionEntity::Kind::Linear;
        dim->axis = {1, 0};
    } else if (dtype == QLatin1String("vertical")) {
        dim->kind = DimensionEntity::Kind::Linear;
        dim->axis = {0, 1};
    } else if (dtype == QLatin1String("radius")) {
        dim->kind = DimensionEntity::Kind::Radius;
    } else if (dtype == QLatin1String("diameter")) {
        dim->kind = DimensionEntity::Kind::Diameter;
    } else if (dtype == QLatin1String("angle")) {
        dim->kind = DimensionEntity::Kind::Angular;
        dim->c = p3;
    } else {
        dim->kind = DimensionEntity::Kind::Aligned; // "parallel", the default
    }
    stage(std::move(dim), layer);
}

void Importer::emitHole(const ENode& n, const Placement& pl)
{
    const double drill = n.num("drill");
    if (drill <= 0)
        return;
    stage(std::make_unique<CircleEntity>(pl.apply({n.num("x"), n.num("y")}),
                                         drill / 2.0),
          45); // Holes
}

void Importer::solidRing(std::vector<Vec2d> ring, int layer)
{
    auto hatch = std::make_unique<HatchEntity>();
    hatch->pattern = QStringLiteral("SOLID");
    hatch->rings.push_back(std::move(ring));
    stage(std::move(hatch), layer);
}

// Ring of the pad/via outer shape, centered on the origin, then rotated.
std::vector<Vec2d> Importer::shapeRing(const QString& shape, double outer,
                                       double rotDeg)
{
    std::vector<Vec2d> ring;
    const double r = outer / 2.0;
    if (shape == QLatin1String("square")) {
        ring = {{-r, -r}, {r, -r}, {r, r}, {-r, r}};
    } else if (shape == QLatin1String("octagon")) {
        const double R = r / std::cos(M_PI / 8.0);
        for (int i = 0; i < 8; ++i) {
            const double a = (22.5 + 45.0 * i) * M_PI / 180.0;
            ring.push_back({R * std::cos(a), R * std::sin(a)});
        }
    } else if (shape == QLatin1String("long") || shape == QLatin1String("offset")) {
        // Obround, total length twice the width (EAGLE's 100% elongation).
        flattenArc({r, 0}, r, -M_PI / 2.0, M_PI, kChordTol, ring);
        flattenArc({-r, 0}, r, M_PI / 2.0, M_PI, kChordTol, ring);
    } else { // round (default)
        flattenArc({0, 0}, r, 0.0, 2.0 * M_PI, kChordTol, ring);
    }
    if (rotDeg != 0.0) {
        const double a = rotDeg * M_PI / 180.0;
        const double ca = std::cos(a), sa = std::sin(a);
        for (auto& p : ring)
            p = {p.x * ca - p.y * sa, p.x * sa + p.y * ca};
    }
    return ring;
}

// Outer ring + drill ring = even-odd annulus on one hatch.
void Importer::annulus(const Vec2d& localCenter, std::vector<Vec2d> outerLocal,
                       double drill, int layer, const Placement& pl)
{
    auto hatch = std::make_unique<HatchEntity>();
    hatch->pattern = QStringLiteral("SOLID");
    for (auto& p : outerLocal)
        p = pl.apply({localCenter.x + p.x, localCenter.y + p.y});
    hatch->rings.push_back(std::move(outerLocal));
    if (drill > 0) {
        std::vector<Vec2d> hole;
        flattenArc({0, 0}, drill / 2.0, 0.0, 2.0 * M_PI, kChordTol, hole);
        for (auto& p : hole)
            p = pl.apply({localCenter.x + p.x, localCenter.y + p.y});
        hatch->rings.push_back(std::move(hole));
    }
    stage(std::move(hatch), layer);
}

void Importer::emitPad(const ENode& n, const Placement& pl)
{
    const double drill = n.num("drill");
    // Absent diameter: EAGLE computes it from the design-rule restring;
    // 1.5x the drill (min +0.5 mm) is the stock look.
    const double outer = n.has("diameter")
        ? n.num("diameter")
        : std::max(drill * 1.5, drill + 0.5);
    const double rot = parseRot(n.attr("rot")).deg;
    annulus({n.num("x"), n.num("y")},
            shapeRing(n.attr("shape", QStringLiteral("round")), outer,
                      pl.mirror ? -rot : rot),
            drill, 17, pl); // Pads
}

void Importer::emitVia(const ENode& n, const Placement& pl)
{
    const double drill = n.num("drill");
    const double outer = n.has("diameter") && n.num("diameter") > 0
        ? n.num("diameter")
        : std::max(drill * 1.5, drill + 0.5);
    annulus({n.num("x"), n.num("y")},
            shapeRing(n.attr("shape", QStringLiteral("round")), outer, 0.0),
            drill, 18, pl); // Vias
}

void Importer::emitSmd(const ENode& n, const Placement& pl)
{
    const double dx = n.num("dx"), dy = n.num("dy");
    const Vec2d c{n.num("x"), n.num("y")};
    const double rot = parseRot(n.attr("rot")).deg * M_PI / 180.0;
    const double ca = std::cos(rot), sa = std::sin(rot);
    std::vector<Vec2d> ring;
    for (const Vec2d& d : {Vec2d{-dx / 2, -dy / 2}, Vec2d{dx / 2, -dy / 2},
                           Vec2d{dx / 2, dy / 2}, Vec2d{-dx / 2, dy / 2}})
        ring.push_back(pl.apply({c.x + d.x * ca - d.y * sa, c.y + d.x * sa + d.y * ca}));
    solidRing(std::move(ring), placedLayer(int(n.num("layer", 1)), pl));
}

void Importer::emitPin(const ENode& n, const Placement& pl)
{
    const Vec2d base{n.num("x"), n.num("y")};
    const QString lenName = n.attr("length", QStringLiteral("long"));
    double len = 7.62;
    if (lenName == QLatin1String("point"))
        len = 0.0;
    else if (lenName == QLatin1String("short"))
        len = 2.54;
    else if (lenName == QLatin1String("middle"))
        len = 5.08;
    const ERot rot = parseRot(n.attr("rot"));
    const double a = rot.deg * M_PI / 180.0;
    const Vec2d dir{std::cos(a), std::sin(a)};
    const Vec2d end = base + dir * len;

    if (len > 0) {
        std::vector<PolyVertex> vs(2);
        vs[0].pos = pl.apply(base);
        vs[1].pos = pl.apply(end);
        auto poly = std::make_unique<PolylineEntity>(std::move(vs), false);
        poly->setWidth(0.1524);
        stage(std::move(poly), 94); // Symbols
    }
    const QString function = n.attr("function");
    if (function.contains(QLatin1String("dot"))) {
        stage(std::make_unique<CircleEntity>(pl.apply(end + dir * 0.7), 0.7),
              94);
    }
    const QString visible = n.attr("visible", QStringLiteral("both"));
    if (visible == QLatin1String("both") || visible == QLatin1String("pin")) {
        TextAlign align;
        align.h = TextHAlign::Left;
        align.v = TextVAlign::Middle;
        ERot textRot;
        textRot.deg = rot.deg;
        emitTextAt(n.attr("name"), end + dir * 0.762, 1.778, 95, textRot,
                   align, pl); // Names
    }
}

void Importer::emitNode(const ENode& n, const Placement& pl,
                        const Subst& subst, bool isBoard)
{
    if (n.name == QLatin1String("wire"))
        emitWire(n, pl);
    else if (n.name == QLatin1String("rectangle"))
        emitRectangle(n, pl);
    else if (n.name == QLatin1String("circle"))
        emitCircle(n, pl);
    else if (n.name == QLatin1String("polygon"))
        emitPolygon(n, pl);
    else if (n.name == QLatin1String("text"))
        emitText(n, pl, subst);
    else if (n.name == QLatin1String("frame"))
        emitFrame(n, pl);
    else if (n.name == QLatin1String("dimension"))
        emitDimension(n, pl);
    else if (n.name == QLatin1String("hole"))
        emitHole(n, pl);
    else if (n.name == QLatin1String("pad") && isBoard)
        emitPad(n, pl);
    else if (n.name == QLatin1String("smd") && isBoard)
        emitSmd(n, pl);
    else if (n.name == QLatin1String("via") && isBoard)
        emitVia(n, pl);
    else if (n.name == QLatin1String("pin") && !isBoard)
        emitPin(n, pl);
    else if (n.name == QLatin1String("description") ||
             n.name == QLatin1String("attribute"))
        ; // meta, handled (or deliberately ignored) by the callers
    else
        skip(n.name);
}

// ---------------------------------------------------------------------------
// Board
// ---------------------------------------------------------------------------

void Importer::runBoard(const ENode& board)
{
    m_result.kind = QStringLiteral("board");

    // Packages by "library\0package".
    QHash<QString, const ENode*> packages;
    if (const ENode* libraries = board.child("libraries")) {
        for (const auto& lib : libraries->children) {
            if (lib.name != QLatin1String("library"))
                continue;
            const QString libName = lib.attr("name");
            if (const ENode* pkgs = lib.child("packages")) {
                for (const auto& p : pkgs->children)
                    if (p.name == QLatin1String("package"))
                        packages.insert(libName + QLatin1Char('\x1f') + p.attr("name"), &p);
            }
        }
    }

    const Subst plain; // no substitution context outside elements
    if (const ENode* pn = board.child("plain")) {
        for (const auto& c : pn->children)
            emitNode(c, Placement{}, plain, true);
    }

    if (const ENode* elements = board.child("elements")) {
        for (const auto& el : elements->children) {
            if (el.name != QLatin1String("element"))
                continue;
            const ENode* pkg = packages.value(
                el.attr("library") + QLatin1Char('\x1f') + el.attr("package"),
                nullptr);
            if (!pkg) {
                skip(QStringLiteral("element (package missing)"));
                continue;
            }
            const ERot rot = parseRot(el.attr("rot"));
            Placement pl;
            pl.pos = {el.num("x"), el.num("y")};
            pl.deg = rot.deg;
            pl.mirror = rot.mirror;

            Subst subst;
            subst.name = el.attr("name");
            subst.part = subst.name;
            subst.value = el.attr("value");
            subst.drawingName = m_result.kind;

            // Smashed name/value: the element's own positioned attributes
            // replace the package's >NAME/>VALUE texts.
            QSet<QString> overridden;
            for (const auto& at : el.children) {
                if (at.name != QLatin1String("attribute") || !at.has("x"))
                    continue;
                const QString key = at.attr("name").toUpper();
                const QString display = at.attr("display", QStringLiteral("value"));
                if (display == QLatin1String("off")) {
                    overridden.insert(key);
                    continue;
                }
                QString shown;
                if (display == QLatin1String("name"))
                    shown = key;
                else if (key == QLatin1String("NAME"))
                    shown = subst.name;
                else if (key == QLatin1String("VALUE"))
                    shown = subst.value;
                else
                    shown = at.attr("value");
                overridden.insert(key);
                if (shown.isEmpty())
                    continue;
                // Attribute placement is absolute (already in board space).
                const ERot arot = parseRot(at.attr("rot"));
                Placement textPl;
                textPl.pos = {at.num("x"), at.num("y")};
                emitTextAt(shown, {0, 0}, at.num("size", 1.778),
                           int(at.num("layer", key == QLatin1String("VALUE") ? 27 : 25)),
                           arot, parseAlign(at.attr("align")), textPl);
            }

            for (const auto& c : pkg->children) {
                if (c.name == QLatin1String("text")) {
                    const QString key = c.text.trimmed().mid(1).toUpper();
                    if (overridden.contains(key))
                        continue;
                }
                emitNode(c, pl, subst, true);
            }
        }
    }

    if (const ENode* sigs = board.child("signals")) {
        for (const auto& sig : sigs->children) {
            if (sig.name != QLatin1String("signal"))
                continue;
            for (const auto& c : sig.children) {
                if (c.name == QLatin1String("contactref"))
                    continue; // connectivity, not graphics
                emitNode(c, Placement{}, plain, true);
            }
        }
    }

    flushStaged({0, 0});
}

// ---------------------------------------------------------------------------
// Schematic
// ---------------------------------------------------------------------------

void Importer::runSchematic(const ENode& schematic)
{
    m_result.kind = QStringLiteral("schematic");

    struct DevGate {
        QString symbol;
    };
    struct DevSet {
        QHash<QString, DevGate> gates;
        int gateCount = 0;
    };
    QHash<QString, const ENode*> symbols;   // "lib\x1fsymbol"
    QHash<QString, DevSet> devsets;         // "lib\x1fdeviceset"

    if (const ENode* libraries = schematic.child("libraries")) {
        for (const auto& lib : libraries->children) {
            if (lib.name != QLatin1String("library"))
                continue;
            const QString libName = lib.attr("name");
            if (const ENode* syms = lib.child("symbols")) {
                for (const auto& s : syms->children)
                    if (s.name == QLatin1String("symbol"))
                        symbols.insert(libName + QLatin1Char('\x1f') + s.attr("name"), &s);
            }
            if (const ENode* dsets = lib.child("devicesets")) {
                for (const auto& ds : dsets->children) {
                    if (ds.name != QLatin1String("deviceset"))
                        continue;
                    DevSet set;
                    if (const ENode* gates = ds.child("gates")) {
                        for (const auto& g : gates->children) {
                            if (g.name != QLatin1String("gate"))
                                continue;
                            set.gates.insert(g.attr("name"),
                                             DevGate{g.attr("symbol")});
                        }
                    }
                    set.gateCount = set.gates.size();
                    devsets.insert(libName + QLatin1Char('\x1f') + ds.attr("name"),
                                   set);
                }
            }
        }
    }

    struct Part {
        QString library, deviceset, value;
    };
    QHash<QString, Part> parts;
    if (const ENode* pn = schematic.child("parts")) {
        for (const auto& p : pn->children) {
            if (p.name != QLatin1String("part"))
                continue;
            Part part;
            part.library = p.attr("library");
            part.deviceset = p.attr("deviceset");
            part.value = p.attr("value");
            if (part.value.isEmpty())
                part.value = part.deviceset;
            parts.insert(p.attr("name"), part);
        }
    }

    const ENode* sheetsNode = schematic.child("sheets");
    if (!sheetsNode)
        return;
    int totalSheets = 0;
    for (const auto& sh : sheetsNode->children)
        if (sh.name == QLatin1String("sheet"))
            ++totalSheets;
    m_result.sheets = totalSheets;

    double cursorX = 0.0;
    bool first = true;
    int sheetNo = 0;
    for (const auto& sheet : sheetsNode->children) {
        if (sheet.name != QLatin1String("sheet"))
            continue;
        ++sheetNo;
        Subst sheetSubst;
        sheetSubst.sheet =
            QStringLiteral("%1/%2").arg(sheetNo).arg(totalSheets);

        if (const ENode* pn = sheet.child("plain")) {
            for (const auto& c : pn->children)
                emitNode(c, Placement{}, sheetSubst, false);
        }

        if (const ENode* instances = sheet.child("instances")) {
            for (const auto& in : instances->children) {
                if (in.name != QLatin1String("instance"))
                    continue;
                const QString partName = in.attr("part");
                const auto pit = parts.constFind(partName);
                if (pit == parts.constEnd()) {
                    skip(QStringLiteral("instance (part missing)"));
                    continue;
                }
                const auto dit = devsets.constFind(
                    pit->library + QLatin1Char('\x1f') + pit->deviceset);
                if (dit == devsets.constEnd()) {
                    skip(QStringLiteral("instance (deviceset missing)"));
                    continue;
                }
                const QString gateName = in.attr("gate");
                const auto git = dit->gates.constFind(gateName);
                const ENode* symbol = (git == dit->gates.constEnd())
                    ? nullptr
                    : symbols.value(pit->library + QLatin1Char('\x1f') + git->symbol,
                                    nullptr);
                if (!symbol) {
                    skip(QStringLiteral("instance (symbol missing)"));
                    continue;
                }
                const ERot rot = parseRot(in.attr("rot"));
                Placement pl;
                pl.pos = {in.num("x"), in.num("y")};
                pl.deg = rot.deg;
                pl.mirror = rot.mirror;

                Subst subst = sheetSubst;
                subst.part = partName;
                subst.gate = gateName;
                subst.name = partName;
                if (dit->gateCount > 1 && !gateName.startsWith(QLatin1String("G$")))
                    subst.name += gateName;
                subst.value = pit->value;

                QSet<QString> overridden;
                for (const auto& at : in.children) {
                    if (at.name != QLatin1String("attribute") || !at.has("x"))
                        continue;
                    const QString key = at.attr("name").toUpper();
                    overridden.insert(key);
                    QString shown;
                    if (key == QLatin1String("NAME"))
                        shown = subst.name;
                    else if (key == QLatin1String("VALUE"))
                        shown = subst.value;
                    else
                        shown = at.attr("value");
                    if (shown.isEmpty())
                        continue;
                    Placement textPl;
                    textPl.pos = {at.num("x"), at.num("y")};
                    emitTextAt(shown, {0, 0}, at.num("size", 1.778),
                               int(at.num("layer", key == QLatin1String("VALUE") ? 96 : 95)),
                               parseRot(at.attr("rot")),
                               parseAlign(at.attr("align")), textPl);
                }

                for (const auto& c : symbol->children) {
                    if (c.name == QLatin1String("text")) {
                        const QString key = c.text.trimmed().mid(1).toUpper();
                        if (overridden.contains(key))
                            continue;
                    }
                    emitNode(c, pl, subst, false);
                }
            }
        }

        auto emitSegments = [&](const ENode& container, const QString& netName,
                                int labelFallbackLayer) {
            for (const auto& seg : container.children) {
                if (seg.name != QLatin1String("segment"))
                    continue;
                for (const auto& c : seg.children) {
                    if (c.name == QLatin1String("pinref") ||
                        c.name == QLatin1String("portref") ||
                        c.name == QLatin1String("probe"))
                        continue; // connectivity only
                    if (c.name == QLatin1String("junction")) {
                        std::vector<Vec2d> ring;
                        flattenArc({c.num("x"), c.num("y")}, 0.508, 0.0,
                                   2.0 * M_PI, kChordTol, ring);
                        solidRing(std::move(ring), 91); // Nets
                        continue;
                    }
                    if (c.name == QLatin1String("label")) {
                        Placement id;
                        emitTextAt(netName, {c.num("x"), c.num("y")},
                                   c.num("size", 1.778),
                                   int(c.num("layer", labelFallbackLayer)),
                                   parseRot(c.attr("rot")), TextAlign{}, id);
                        continue;
                    }
                    emitNode(c, Placement{}, sheetSubst, false);
                }
            }
        };

        if (const ENode* nets = sheet.child("nets")) {
            for (const auto& net : nets->children)
                if (net.name == QLatin1String("net"))
                    emitSegments(net, net.attr("name"), 95);
        }
        if (const ENode* busses = sheet.child("busses")) {
            for (const auto& bus : busses->children)
                if (bus.name == QLatin1String("bus"))
                    emitSegments(bus, bus.attr("name"), 92);
        }

        // Lay sheets side by side: shift so this sheet starts at cursorX.
        const BBox2d box = stagedBounds();
        Vec2d offset{0, 0};
        if (!first && box.isValid())
            offset.x = cursorX - box.min.x;
        if (box.isValid())
            cursorX = box.max.x + offset.x + kSheetGap;
        first = false;
        flushStaged(offset);
    }
}

// ---------------------------------------------------------------------------
// Library: every package and symbol becomes one cell of a grid, its name
// printed underneath on a dedicated "Labels" layer. >NAME/>VALUE stay
// literal, exactly like EAGLE's own library editor shows them.
// ---------------------------------------------------------------------------

void Importer::runLibrary(const ENode& library)
{
    m_result.kind = QStringLiteral("library");

    Subst literal;
    literal.name = QStringLiteral(">NAME");
    literal.value = QStringLiteral(">VALUE");
    literal.part = QStringLiteral(">PART");
    literal.gate = QStringLiteral(">GATE");

    struct Cell {
        std::vector<std::unique_ptr<Entity>> entities;
        BBox2d box;
    };
    std::vector<Cell> cells;

    const LayerId labels = m_doc->ensureLayer(QStringLiteral("Labels"), 0xFFFF64);
    m_doc->setLayerRank(labels, 40);
    if (m_anyLayer == 0)
        m_anyLayer = labels;

    auto harvest = [&](const ENode& item, bool isBoard) {
        for (const auto& c : item.children)
            emitNode(c, Placement{}, literal, isBoard);
        BBox2d box;
        for (const auto& e : m_staged)
            box.expand(e->bounds());
        if (!box.isValid())
            box = BBox2d({-1, -1}, {1, 1});
        auto title = std::make_unique<TextEntity>(
            Vec2d{box.center().x, box.min.y - 1.5}, 2.0, 0.0, item.attr("name"));
        title->hAlign = TextHAlign::Center;
        title->vAlign = TextVAlign::Top;
        title->setLayerId(labels);
        box.expand(title->bounds());
        m_staged.push_back(std::move(title));
        ++m_result.imported;
        Cell cell;
        cell.entities = std::move(m_staged);
        m_staged.clear();
        cell.box = box;
        cells.push_back(std::move(cell));
    };

    if (const ENode* pkgs = library.child("packages")) {
        for (const auto& p : pkgs->children)
            if (p.name == QLatin1String("package"))
                harvest(p, true);
    }
    if (const ENode* syms = library.child("symbols")) {
        for (const auto& sy : syms->children)
            if (sy.name == QLatin1String("symbol"))
                harvest(sy, false);
    }
    // Devicesets only wire gates to symbols already shown — nothing to draw.

    if (cells.empty())
        return; // an empty library imports as an empty document

    // Row-flow grid: ~square cell count, gap sized from the biggest cell.
    const int cols = std::max(1, int(std::ceil(std::sqrt(double(cells.size())))));
    double gap = 0.0;
    for (const auto& c : cells)
        gap = std::max({gap, c.box.width(), c.box.height()});
    gap = std::max(5.0, gap * 0.15);

    double rowY = 0.0; // top edge of the current row
    double cursorX = 0.0;
    double rowDepth = 0.0;
    int inRow = 0;
    for (auto& cell : cells) {
        if (inRow == cols) {
            rowY -= rowDepth + gap;
            cursorX = 0.0;
            rowDepth = 0.0;
            inRow = 0;
        }
        const Vec2d offset{cursorX - cell.box.min.x, rowY - cell.box.max.y};
        for (auto& e : cell.entities) {
            e->transform(Xform2d::translation(offset));
            m_doc->restoreEntity(std::move(e), m_doc->nextId());
            m_doc->setNextId(m_doc->nextId() + 1);
        }
        cursorX += cell.box.width() + gap;
        rowDepth = std::max(rowDepth, cell.box.height());
        ++inRow;
    }
}

// ---------------------------------------------------------------------------

void Importer::run(const ENode& drawing, const ENode& section, Kind kind)
{
    if (const ENode* layers = drawing.child("layers")) {
        for (const auto& l : layers->children) {
            if (l.name != QLatin1String("layer"))
                continue;
            FileLayer fl;
            fl.name = l.attr("name");
            fl.rgb = kPalette[int(l.num("color", 7)) & 15];
            fl.visible = l.attr("visible", QStringLiteral("yes")) != QLatin1String("no");
            m_layerTable.insert(int(l.num("number")), fl);
        }
    }

    switch (kind) {
    case Kind::Board:
        runBoard(section);
        break;
    case Kind::Schematic:
        runSchematic(section);
        break;
    case Kind::Library:
        runLibrary(section);
        break;
    }

    if (m_anyLayer != 0) {
        m_doc->setCurrentLayer(m_anyLayer);
        m_doc->dropEmptyLayerZero();
    }
    m_result.ok = true;
}

} // namespace

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

bool isEagleBinary(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(4);
    return head.size() >= 1 && quint8(head.at(0)) == 0x10;
}

bool isEagleXml(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(2048);
    return head.contains("<eagle ") || head.contains("<eagle>") ||
           head.contains("DOCTYPE eagle");
}

EagleImportResult importEagle(const QString& path)
{
    EagleImportResult result;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("cannot open: %1").arg(path);
        return result;
    }
    const QByteArray data = f.readAll();
    if (!data.isEmpty() && quint8(data.at(0)) == 0x10) {
        result.error = QStringLiteral(
            "binary EAGLE file (version 5 or older): %1 — open it in EAGLE 6+ "
            "and save to convert it to XML").arg(path);
        return result;
    }
    // Not XML at all (e.g. a .sch from another EDA tool): say so instead of
    // letting the XML parser complain about encodings.
    int first = 0;
    if (data.startsWith("\xEF\xBB\xBF"))
        first = 3; // UTF-8 BOM
    while (first < data.size() && std::isspace(uchar(data.at(first))))
        ++first;
    if (first >= data.size() || data.at(first) != '<') {
        result.error = QStringLiteral("not an EAGLE XML file: %1").arg(path);
        return result;
    }

    ENode root;
    {
        QXmlStreamReader xml(data);
        while (!xml.atEnd()) {
            if (xml.readNext() == QXmlStreamReader::StartElement) {
                readNode(xml, root);
                break;
            }
        }
        if (xml.hasError()) {
            result.error = QStringLiteral("XML error line %1: %2")
                               .arg(xml.lineNumber())
                               .arg(xml.errorString());
            return result;
        }
    }
    if (root.name != QLatin1String("eagle")) {
        result.error = QStringLiteral("not an EAGLE XML file: %1").arg(path);
        return result;
    }
    const ENode* drawing = root.child("drawing");
    if (!drawing) {
        result.error = QStringLiteral("EAGLE file has no <drawing>: %1").arg(path);
        return result;
    }
    const ENode* board = drawing->child("board");
    const ENode* schematic = drawing->child("schematic");
    const ENode* library = drawing->child("library");
    if (!board && !schematic && !library) {
        result.error = QStringLiteral(
            "EAGLE drawing has no board, schematic or library: %1").arg(path);
        return result;
    }

    Importer importer(result);
    if (board)
        importer.run(*drawing, *board, Importer::Kind::Board);
    else if (schematic)
        importer.run(*drawing, *schematic, Importer::Kind::Schematic);
    else
        importer.run(*drawing, *library, Importer::Kind::Library);
    return result;
}

} // namespace viki
