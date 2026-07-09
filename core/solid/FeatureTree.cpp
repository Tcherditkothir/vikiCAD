#include "solid/FeatureTree.h"

#include <sstream>

#include <QJsonArray>
#include <QJsonValue>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BinTools.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <GC_MakeSegment.hxx>
#include <Geom_Circle.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

namespace viki {

namespace {

// Map a 2D work-plane point (u,v) into 3D, matching SolidOps::to3d exactly so
// the wires this tree builds sit on the same plane as the rest of the pipeline.
gp_Pnt to3d(const Vec2d& p, const WorkPlane& plane)
{
    const gp_Vec x(plane.xDir);
    const gp_Vec y(plane.normal.Crossed(plane.xDir));
    return plane.origin.Translated(x * p.x + y * p.y);
}

// Build a closed wire from an ordered ring of 3D points (last -> first closes).
bool wireFromRing(const std::vector<gp_Pnt>& pts, TopoDS_Wire& out, QString& err)
{
    if (pts.size() < 3) {
        err = QStringLiteral("polygon profile needs at least 3 vertices");
        return false;
    }
    BRepBuilderAPI_MakeWire maker;
    const size_t n = pts.size();
    for (size_t i = 0; i < n; ++i) {
        const gp_Pnt& a = pts[i];
        const gp_Pnt& b = pts[(i + 1) % n];
        if (a.Distance(b) < 1e-9) {
            err = QStringLiteral("polygon profile has a degenerate edge");
            return false;
        }
        const auto seg = GC_MakeSegment(a, b);
        if (!seg.IsDone()) {
            err = QStringLiteral("failed to build profile segment");
            return false;
        }
        maker.Add(BRepBuilderAPI_MakeEdge(seg.Value()).Edge());
    }
    if (!maker.IsDone()) {
        err = QStringLiteral("failed to close profile wire");
        return false;
    }
    out = maker.Wire();
    return true;
}

bool wireForProfile(const SketchProfile& prof, const WorkPlane& plane,
                    TopoDS_Wire& out, QString& err)
{
    switch (prof.kind) {
    case ProfileKind::Rectangle: {
        const Vec2d& lo = prof.rectMin;
        const Vec2d& hi = prof.rectMax;
        if (nearEqual(lo.x, hi.x) || nearEqual(lo.y, hi.y)) {
            err = QStringLiteral("rectangle profile has zero extent");
            return false;
        }
        const std::vector<gp_Pnt> ring = {
            to3d({lo.x, lo.y}, plane),
            to3d({hi.x, lo.y}, plane),
            to3d({hi.x, hi.y}, plane),
            to3d({lo.x, hi.y}, plane),
        };
        return wireFromRing(ring, out, err);
    }
    case ProfileKind::Circle: {
        if (prof.circleRadius <= 1e-9) {
            err = QStringLiteral("circle profile has non-positive radius");
            return false;
        }
        const gp_Pnt c = to3d(prof.circleCenter, plane);
        const gp_Ax2 ax(c, plane.normal, plane.xDir);
        const gp_Circ circ(ax, prof.circleRadius);
        BRepBuilderAPI_MakeWire maker;
        maker.Add(BRepBuilderAPI_MakeEdge(circ).Edge());
        if (!maker.IsDone()) {
            err = QStringLiteral("failed to build circle wire");
            return false;
        }
        out = maker.Wire();
        return true;
    }
    case ProfileKind::Polygon: {
        std::vector<gp_Pnt> ring;
        ring.reserve(prof.polygon.size());
        for (const Vec2d& v : prof.polygon)
            ring.push_back(to3d(v, plane));
        return wireFromRing(ring, out, err);
    }
    }
    err = QStringLiteral("unknown profile kind");
    return false;
}

} // namespace

int FeatureTree::addNode(FeatureNode node)
{
    m_nodes.push_back(std::move(node));
    return static_cast<int>(m_nodes.size()) - 1;
}

bool FeatureTree::setExtrudeHeight(int i, double height)
{
    if (i < 0 || i >= count())
        return false;
    FeatureNode& n = m_nodes[static_cast<size_t>(i)];
    if (n.kind != FeatureKind::Extrude)
        return false;
    n.height = height;
    return true;
}

bool FeatureTree::setHoleDiameter(int i, double diameter)
{
    if (i < 0 || i >= count())
        return false;
    FeatureNode& n = m_nodes[static_cast<size_t>(i)];
    if (n.kind != FeatureKind::Hole)
        return false;
    n.diameter = diameter;
    return true;
}

bool FeatureTree::setHoleDepth(int i, double depth)
{
    if (i < 0 || i >= count())
        return false;
    FeatureNode& n = m_nodes[static_cast<size_t>(i)];
    if (n.kind != FeatureKind::Hole)
        return false;
    n.depth = depth;
    return true;
}

bool FeatureTree::setHoleCenter(int i, const Vec2d& center)
{
    if (i < 0 || i >= count())
        return false;
    FeatureNode& n = m_nodes[static_cast<size_t>(i)];
    if (n.kind != FeatureKind::Hole)
        return false;
    n.holeCenter = center;
    return true;
}

bool FeatureTree::setShellThickness(int i, double thickness)
{
    if (i < 0 || i >= count())
        return false;
    FeatureNode& n = m_nodes[static_cast<size_t>(i)];
    if (n.kind != FeatureKind::Shell)
        return false;
    n.thickness = thickness;
    return true;
}

bool FeatureTree::wiresForSketch(const FeatureNode& sketch,
                                 std::vector<TopoDS_Wire>& out, QString& err)
{
    if (sketch.kind != FeatureKind::Sketch) {
        err = QStringLiteral("node is not a Sketch");
        return false;
    }
    if (sketch.profiles.empty()) {
        err = QStringLiteral("sketch has no profiles");
        return false;
    }
    out.clear();
    for (const SketchProfile& prof : sketch.profiles) {
        TopoDS_Wire w;
        if (!wireForProfile(prof, sketch.plane, w, err))
            return false;
        out.push_back(w);
    }
    return true;
}

RegenResult FeatureTree::regenerate() const
{
    RegenResult result;

    // Per-node shape cache so Join/Cut extrudes can reference an earlier
    // solid-producing node's output. Sketch nodes leave a null shape.
    std::vector<TopoDS_Shape> shapes(m_nodes.size());
    int lastSolid = -1;

    for (int i = 0; i < count(); ++i) {
        const FeatureNode& node = m_nodes[static_cast<size_t>(i)];
        switch (node.kind) {
        case FeatureKind::Sketch:
            // Sketches carry no solid; validate them lazily when consumed.
            break;
        case FeatureKind::Extrude: {
            // Resolve the sketch this extrude consumes.
            int si = node.sketchIndex;
            if (si < 0) {
                for (int j = i - 1; j >= 0; --j) {
                    if (m_nodes[static_cast<size_t>(j)].kind == FeatureKind::Sketch) {
                        si = j;
                        break;
                    }
                }
            }
            if (si < 0 || si >= count() ||
                m_nodes[static_cast<size_t>(si)].kind != FeatureKind::Sketch) {
                result.message = QStringLiteral("extrude references no sketch");
                return result;
            }

            std::vector<TopoDS_Wire> wires;
            if (!wiresForSketch(m_nodes[static_cast<size_t>(si)], wires, result.message))
                return result;

            solidops::SolidResult sr;
            if (node.mode == solidops::ExtrudeMode::NewBody ||
                node.mode == solidops::ExtrudeMode::Symmetric) {
                sr = solidops::extrudeWires(wires, node.height,
                                            m_nodes[static_cast<size_t>(si)].plane,
                                            node.mode, TopoDS_Shape());
            } else {
                // Join/Cut: resolve the target shape.
                int ti = node.targetIndex;
                if (ti < 0)
                    ti = lastSolid;
                if (ti < 0 || ti >= count() || shapes[static_cast<size_t>(ti)].IsNull()) {
                    result.message = QStringLiteral("join/cut extrude has no target solid");
                    return result;
                }
                sr = solidops::extrudeWires(wires, node.height,
                                            m_nodes[static_cast<size_t>(si)].plane,
                                            node.mode, shapes[static_cast<size_t>(ti)]);
            }
            if (!sr.ok) {
                result.message = sr.message;
                return result;
            }
            if (sr.shape.IsNull()) {
                result.message = QStringLiteral("extrude produced a null shape");
                return result;
            }
            shapes[static_cast<size_t>(i)] = sr.shape;
            lastSolid = i;
            break;
        }
        case FeatureKind::BaseShape:
            if (node.baseShape.IsNull()) {
                result.message = QStringLiteral("base shape node is empty");
                return result;
            }
            shapes[static_cast<size_t>(i)] = node.baseShape;
            lastSolid = i;
            break;
        case FeatureKind::Hole:
        case FeatureKind::Shell: {
            // Both transform the CURRENT body (the last solid-producing node).
            if (lastSolid < 0) {
                result.message =
                    QStringLiteral("hole/shell has no solid to act on");
                return result;
            }
            const TopoDS_Shape& current = shapes[static_cast<size_t>(lastSolid)];
            const solidops::SolidResult sr =
                node.kind == FeatureKind::Hole
                    ? solidops::makeHole(current, node.plane, node.holeCenter,
                                         node.diameter, node.depth, node.through)
                    : solidops::shellSolid(current, node.thickness);
            if (!sr.ok || sr.shape.IsNull()) {
                result.message = sr.message.isEmpty()
                                     ? QStringLiteral("feature replay failed")
                                     : sr.message;
                return result;
            }
            shapes[static_cast<size_t>(i)] = sr.shape;
            lastSolid = i;
            break;
        }
        }
    }

    if (lastSolid < 0) {
        result.message = QStringLiteral("feature tree produced no solid");
        return result;
    }
    result.ok = true;
    result.shape = shapes[static_cast<size_t>(lastSolid)];
    return result;
}

// ---------------------------------------------------------------------------
// JSON round-trip. Node kinds/extrude modes as short strings, work planes as
// nine full-precision doubles (same layout as the .vkd workplane meta), the
// BaseShape brep as BinTools base64 (same stream SolidEntity persists).
// ---------------------------------------------------------------------------

namespace {

QJsonArray planeToJson(const WorkPlane& wp)
{
    return QJsonArray{wp.origin.X(), wp.origin.Y(), wp.origin.Z(),
                      wp.normal.X(), wp.normal.Y(), wp.normal.Z(),
                      wp.xDir.X(),   wp.xDir.Y(),   wp.xDir.Z()};
}

bool planeFromJson(const QJsonValue& v, WorkPlane& out)
{
    const QJsonArray a = v.toArray();
    if (a.size() != 9)
        return false;
    double c[9];
    for (int i = 0; i < 9; ++i)
        c[i] = a.at(i).toDouble();
    // gp_Dir throws on zero-magnitude vectors; guard before constructing.
    if (c[3] * c[3] + c[4] * c[4] + c[5] * c[5] <= 1e-18 ||
        c[6] * c[6] + c[7] * c[7] + c[8] * c[8] <= 1e-18)
        return false;
    out = WorkPlane{gp_Pnt(c[0], c[1], c[2]), gp_Dir(c[3], c[4], c[5]),
                    gp_Dir(c[6], c[7], c[8])};
    return true;
}

QString shapeToBase64(const TopoDS_Shape& shape)
{
    std::ostringstream stream;
    BinTools::Write(shape, stream);
    const std::string data = stream.str();
    return QString::fromLatin1(
        QByteArray(data.data(), int(data.size())).toBase64());
}

TopoDS_Shape shapeFromBase64(const QString& b64)
{
    const QByteArray bytes = QByteArray::fromBase64(b64.toLatin1());
    TopoDS_Shape shape;
    if (!bytes.isEmpty()) {
        std::istringstream stream(
            std::string(bytes.constData(), size_t(bytes.size())));
        BinTools::Read(shape, stream);
    }
    return shape;
}

QJsonObject profileToJson(const SketchProfile& p)
{
    QJsonObject o;
    switch (p.kind) {
    case ProfileKind::Rectangle:
        o[QStringLiteral("kind")] = QStringLiteral("rect");
        o[QStringLiteral("min")] = QJsonArray{p.rectMin.x, p.rectMin.y};
        o[QStringLiteral("max")] = QJsonArray{p.rectMax.x, p.rectMax.y};
        break;
    case ProfileKind::Circle:
        o[QStringLiteral("kind")] = QStringLiteral("circle");
        o[QStringLiteral("center")] = QJsonArray{p.circleCenter.x, p.circleCenter.y};
        o[QStringLiteral("radius")] = p.circleRadius;
        break;
    case ProfileKind::Polygon: {
        o[QStringLiteral("kind")] = QStringLiteral("poly");
        QJsonArray pts;
        for (const Vec2d& v : p.polygon) {
            pts.append(v.x);
            pts.append(v.y);
        }
        o[QStringLiteral("pts")] = pts;
        break;
    }
    }
    return o;
}

bool vec2FromJson(const QJsonValue& v, Vec2d& out)
{
    const QJsonArray a = v.toArray();
    if (a.size() != 2)
        return false;
    out = {a.at(0).toDouble(), a.at(1).toDouble()};
    return true;
}

bool profileFromJson(const QJsonObject& o, SketchProfile& out)
{
    const QString kind = o[QStringLiteral("kind")].toString();
    if (kind == QLatin1String("rect")) {
        Vec2d lo, hi;
        if (!vec2FromJson(o[QStringLiteral("min")], lo) ||
            !vec2FromJson(o[QStringLiteral("max")], hi))
            return false;
        out = SketchProfile::rectangle(lo, hi);
        return true;
    }
    if (kind == QLatin1String("circle")) {
        Vec2d c;
        if (!vec2FromJson(o[QStringLiteral("center")], c))
            return false;
        out = SketchProfile::circle(c, o[QStringLiteral("radius")].toDouble());
        return true;
    }
    if (kind == QLatin1String("poly")) {
        const QJsonArray pts = o[QStringLiteral("pts")].toArray();
        if (pts.size() < 6 || pts.size() % 2 != 0)
            return false;
        std::vector<Vec2d> verts;
        verts.reserve(size_t(pts.size()) / 2);
        for (int i = 0; i + 1 < pts.size(); i += 2)
            verts.push_back({pts.at(i).toDouble(), pts.at(i + 1).toDouble()});
        out = SketchProfile::polygonFrom(std::move(verts));
        return true;
    }
    return false;
}

QString modeToString(solidops::ExtrudeMode m)
{
    switch (m) {
    case solidops::ExtrudeMode::NewBody: return QStringLiteral("new");
    case solidops::ExtrudeMode::Join: return QStringLiteral("join");
    case solidops::ExtrudeMode::Cut: return QStringLiteral("cut");
    case solidops::ExtrudeMode::Symmetric: return QStringLiteral("symmetric");
    }
    return QStringLiteral("new");
}

bool modeFromString(const QString& s, solidops::ExtrudeMode& out)
{
    if (s == QLatin1String("new")) out = solidops::ExtrudeMode::NewBody;
    else if (s == QLatin1String("join")) out = solidops::ExtrudeMode::Join;
    else if (s == QLatin1String("cut")) out = solidops::ExtrudeMode::Cut;
    else if (s == QLatin1String("symmetric")) out = solidops::ExtrudeMode::Symmetric;
    else return false;
    return true;
}

} // namespace

QJsonObject FeatureTree::toJson() const
{
    QJsonArray nodes;
    for (const FeatureNode& n : m_nodes) {
        QJsonObject o;
        switch (n.kind) {
        case FeatureKind::Sketch: {
            o[QStringLiteral("kind")] = QStringLiteral("sketch");
            o[QStringLiteral("plane")] = planeToJson(n.plane);
            QJsonArray profiles;
            for (const SketchProfile& p : n.profiles)
                profiles.append(profileToJson(p));
            o[QStringLiteral("profiles")] = profiles;
            break;
        }
        case FeatureKind::Extrude:
            o[QStringLiteral("kind")] = QStringLiteral("extrude");
            o[QStringLiteral("height")] = n.height;
            o[QStringLiteral("sketch")] = n.sketchIndex;
            o[QStringLiteral("mode")] = modeToString(n.mode);
            o[QStringLiteral("target")] = n.targetIndex;
            break;
        case FeatureKind::BaseShape:
            o[QStringLiteral("kind")] = QStringLiteral("base");
            o[QStringLiteral("brep")] = shapeToBase64(n.baseShape);
            break;
        case FeatureKind::Hole:
            o[QStringLiteral("kind")] = QStringLiteral("hole");
            o[QStringLiteral("plane")] = planeToJson(n.plane);
            o[QStringLiteral("center")] = QJsonArray{n.holeCenter.x, n.holeCenter.y};
            o[QStringLiteral("diameter")] = n.diameter;
            o[QStringLiteral("depth")] = n.depth;
            o[QStringLiteral("through")] = n.through;
            break;
        case FeatureKind::Shell:
            o[QStringLiteral("kind")] = QStringLiteral("shell");
            o[QStringLiteral("thickness")] = n.thickness;
            break;
        }
        nodes.append(o);
    }
    QJsonObject root;
    root[QStringLiteral("nodes")] = nodes;
    return root;
}

bool FeatureTree::fromJson(const QJsonObject& obj)
{
    m_nodes.clear();
    const QJsonValue nv = obj[QStringLiteral("nodes")];
    if (!nv.isArray())
        return false;
    for (const QJsonValue& v : nv.toArray()) {
        const QJsonObject o = v.toObject();
        const QString kind = o[QStringLiteral("kind")].toString();
        FeatureNode n;
        if (kind == QLatin1String("sketch")) {
            n.kind = FeatureKind::Sketch;
            if (!planeFromJson(o[QStringLiteral("plane")], n.plane)) {
                m_nodes.clear();
                return false;
            }
            for (const QJsonValue& pv : o[QStringLiteral("profiles")].toArray()) {
                SketchProfile p;
                if (!profileFromJson(pv.toObject(), p)) {
                    m_nodes.clear();
                    return false;
                }
                n.profiles.push_back(std::move(p));
            }
        } else if (kind == QLatin1String("extrude")) {
            n.kind = FeatureKind::Extrude;
            n.height = o[QStringLiteral("height")].toDouble();
            n.sketchIndex = o[QStringLiteral("sketch")].toInt(-1);
            n.targetIndex = o[QStringLiteral("target")].toInt(-1);
            if (!modeFromString(o[QStringLiteral("mode")].toString(), n.mode)) {
                m_nodes.clear();
                return false;
            }
        } else if (kind == QLatin1String("base")) {
            n.kind = FeatureKind::BaseShape;
            n.baseShape = shapeFromBase64(o[QStringLiteral("brep")].toString());
            if (n.baseShape.IsNull()) {
                m_nodes.clear();
                return false;
            }
        } else if (kind == QLatin1String("hole")) {
            n.kind = FeatureKind::Hole;
            if (!planeFromJson(o[QStringLiteral("plane")], n.plane) ||
                !vec2FromJson(o[QStringLiteral("center")], n.holeCenter)) {
                m_nodes.clear();
                return false;
            }
            n.diameter = o[QStringLiteral("diameter")].toDouble();
            n.depth = o[QStringLiteral("depth")].toDouble();
            n.through = o[QStringLiteral("through")].toBool();
        } else if (kind == QLatin1String("shell")) {
            n.kind = FeatureKind::Shell;
            n.thickness = o[QStringLiteral("thickness")].toDouble();
        } else {
            m_nodes.clear();
            return false;
        }
        m_nodes.push_back(std::move(n));
    }
    return true;
}

} // namespace viki
