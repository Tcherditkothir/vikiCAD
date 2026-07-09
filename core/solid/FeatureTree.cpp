#include "solid/FeatureTree.h"

#include <BRepBuilderAPI_MakeEdge.hxx>
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

} // namespace viki
