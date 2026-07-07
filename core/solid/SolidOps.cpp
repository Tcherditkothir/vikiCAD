#include "SolidOps.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GC_MakeSegment.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "geom/GeomUtil.h"

namespace viki {
namespace solidops {
namespace {

gp_Pnt to3d(const Vec2d& p, const WorkPlane& plane)
{
    return {p.x, p.y, plane.zOffset};
}

// Segment/arc pieces of a profile, oriented a -> b.
struct ProfilePiece {
    Vec2d a, b;
    bool isArc = false;
    Vec2d center;
    bool ccw = true;
    Vec2d mid; // point on the arc between a and b
};

void piecesFromEntity(const Entity& e, std::vector<std::vector<ProfilePiece>>& loops,
                      std::vector<ProfilePiece>& open)
{
    if (const auto* line = dynamic_cast<const LineEntity*>(&e)) {
        open.push_back({line->p1(), line->p2(), false, {}, true, {}});
    } else if (const auto* arc = dynamic_cast<const ArcEntity*>(&e)) {
        ProfilePiece p;
        p.a = arc->startPoint();
        p.b = arc->endPoint();
        p.isArc = true;
        p.center = arc->center();
        p.mid = arc->center() +
                Vec2d::polar(arc->radius(), arc->startAngle() + arc->sweep() / 2);
        open.push_back(p);
    } else if (const auto* circle = dynamic_cast<const CircleEntity*>(&e)) {
        // A full circle is a closed loop by itself (two half arcs).
        ProfilePiece p1, p2;
        const Vec2d c = circle->center();
        const double r = circle->radius();
        p1 = {c + Vec2d{r, 0}, c - Vec2d{r, 0}, true, c, true, c + Vec2d{0, r}};
        p2 = {c - Vec2d{r, 0}, c + Vec2d{r, 0}, true, c, true, c - Vec2d{0, r}};
        loops.push_back({p1, p2});
    } else if (const auto* pl = dynamic_cast<const PolylineEntity*>(&e)) {
        std::vector<ProfilePiece> loop;
        const auto& vs = pl->vertices();
        const size_t n = vs.size();
        const size_t segCount = pl->isClosed() ? n : n - 1;
        for (size_t i = 0; i < segCount; ++i) {
            const Vec2d a = vs[i].pos;
            const Vec2d b = vs[(i + 1) % n].pos;
            const double bulge = vs[i].bulge;
            if (nearZero(bulge)) {
                loop.push_back({a, b, false, {}, true, {}});
            } else {
                const double sweep = 4.0 * std::atan(bulge);
                const double chord = a.distanceTo(b);
                const double radius = chord / (2.0 * std::fabs(std::sin(sweep / 2.0)));
                const Vec2d mid = (a + b) * 0.5;
                const double h = radius * std::cos(sweep / 2.0) * (bulge > 0 ? 1 : -1);
                const Vec2d center = mid + (b - a).normalized().perp() * h;
                ProfilePiece p;
                p.a = a;
                p.b = b;
                p.isArc = true;
                p.center = center;
                const double midAngle = (a - center).angle() + sweep / 2.0;
                p.mid = center + Vec2d::polar(radius, midAngle);
                loop.push_back(p);
            }
        }
        if (pl->isClosed())
            loops.push_back(std::move(loop));
        else
            for (auto& p : loop)
                open.push_back(p);
    } else if (const auto* el = dynamic_cast<const EllipseEntity*>(&e)) {
        if (el->isFull()) {
            // Approximate with a fine polyline loop (proper Geom_Ellipse later).
            RenderContext rc;
            rc.chordTolerance = 0.01;
            PrimitiveList list;
            el->buildPrimitives(rc, list);
            if (!list.strokes.empty()) {
                std::vector<ProfilePiece> loop;
                const auto& pts = list.strokes[0].points;
                for (size_t i = 0; i + 1 < pts.size(); ++i)
                    loop.push_back({pts[i], pts[i + 1], false, {}, true, {}});
                loop.push_back({pts.back(), pts.front(), false, {}, true, {}});
                loops.push_back(std::move(loop));
            }
        }
    }
}

// Chain open pieces into closed loops (tolerance 1e-6).
bool chainOpenPieces(std::vector<ProfilePiece> open,
                     std::vector<std::vector<ProfilePiece>>& loops)
{
    constexpr double tol = 1e-6;
    while (!open.empty()) {
        std::vector<ProfilePiece> chain{open.front()};
        open.erase(open.begin());
        bool grew = true;
        while (grew) {
            grew = false;
            for (size_t i = 0; i < open.size(); ++i) {
                ProfilePiece p = open[i];
                if (nearEqual(chain.back().b, p.a, tol)) {
                    chain.push_back(p);
                } else if (nearEqual(chain.back().b, p.b, tol)) {
                    std::swap(p.a, p.b);
                    chain.push_back(p);
                } else {
                    continue;
                }
                open.erase(open.begin() + long(i));
                grew = true;
                break;
            }
        }
        if (!nearEqual(chain.front().a, chain.back().b, tol))
            return false; // open chain — not a valid profile
        loops.push_back(std::move(chain));
    }
    return true;
}

TopoDS_Wire wireFromLoop(const std::vector<ProfilePiece>& loop, const WorkPlane& plane)
{
    BRepBuilderAPI_MakeWire maker;
    for (const ProfilePiece& p : loop) {
        if (p.isArc) {
            const auto arc =
                GC_MakeArcOfCircle(to3d(p.a, plane), to3d(p.mid, plane), to3d(p.b, plane));
            maker.Add(BRepBuilderAPI_MakeEdge(arc.Value()).Edge());
        } else {
            if (p.a.distanceTo(p.b) < 1e-9)
                continue;
            const auto seg = GC_MakeSegment(to3d(p.a, plane), to3d(p.b, plane));
            maker.Add(BRepBuilderAPI_MakeEdge(seg.Value()).Edge());
        }
    }
    return maker.IsDone() ? maker.Wire() : TopoDS_Wire();
}

} // namespace

WireResult wiresFromEntities(const Document& doc, const std::vector<EntityId>& ids,
                             const WorkPlane& plane)
{
    WireResult result;
    std::vector<std::vector<ProfilePiece>> loops;
    std::vector<ProfilePiece> open;
    for (const EntityId id : ids) {
        const Entity* e = doc.entity(id);
        if (!e) {
            result.message = QStringLiteral("entity %1 not found").arg(id);
            return result;
        }
        piecesFromEntity(*e, loops, open);
    }
    if (!chainOpenPieces(std::move(open), loops)) {
        result.message =
            QStringLiteral("profile is not closed (chain the lines/arcs end to end)");
        return result;
    }
    if (loops.empty()) {
        result.message = QStringLiteral("no usable profile (need closed shapes)");
        return result;
    }
    for (const auto& loop : loops) {
        const TopoDS_Wire wire = wireFromLoop(loop, plane);
        if (wire.IsNull()) {
            result.message = QStringLiteral("failed to build a wire from the profile");
            return result;
        }
        result.wires.push_back(wire);
    }
    result.ok = true;
    return result;
}

SolidResult extrudeWires(const std::vector<TopoDS_Wire>& wires, double height)
{
    SolidResult result;
    if (nearZero(height)) {
        result.message = QStringLiteral("height must be non-zero");
        return result;
    }
    TopoDS_Shape acc;
    for (const TopoDS_Wire& wire : wires) {
        BRepBuilderAPI_MakeFace face(wire);
        if (!face.IsDone()) {
            result.message = QStringLiteral("profile wire does not bound a face");
            return result;
        }
        BRepPrimAPI_MakePrism prism(face.Face(), gp_Vec(0, 0, height));
        if (!prism.IsDone()) {
            result.message = QStringLiteral("extrusion failed");
            return result;
        }
        if (acc.IsNull()) {
            acc = prism.Shape();
        } else {
            BRepAlgoAPI_Fuse fuse(acc, prism.Shape());
            if (!fuse.IsDone()) {
                result.message = QStringLiteral("fusing profiles failed");
                return result;
            }
            acc = fuse.Shape();
        }
    }
    result.ok = true;
    result.shape = acc;
    return result;
}

SolidResult revolveWires(const std::vector<TopoDS_Wire>& wires, const Vec2d& axisA,
                         const Vec2d& axisB, double angle, const WorkPlane& plane)
{
    SolidResult result;
    if (axisA.distanceTo(axisB) < 1e-9) {
        result.message = QStringLiteral("degenerate revolution axis");
        return result;
    }
    const Vec2d dir = (axisB - axisA).normalized();
    const gp_Ax1 axis(to3d(axisA, plane), gp_Dir(dir.x, dir.y, 0));
    TopoDS_Shape acc;
    for (const TopoDS_Wire& wire : wires) {
        BRepBuilderAPI_MakeFace face(wire);
        if (!face.IsDone()) {
            result.message = QStringLiteral("profile wire does not bound a face");
            return result;
        }
        BRepPrimAPI_MakeRevol revol(face.Face(), axis, angle);
        if (!revol.IsDone()) {
            result.message = QStringLiteral("revolution failed (profile crossing the axis?)");
            return result;
        }
        if (acc.IsNull()) {
            acc = revol.Shape();
        } else {
            BRepAlgoAPI_Fuse fuse(acc, revol.Shape());
            if (!fuse.IsDone()) {
                result.message = QStringLiteral("fusing profiles failed");
                return result;
            }
            acc = fuse.Shape();
        }
    }
    result.ok = true;
    result.shape = acc;
    return result;
}

SolidResult booleanOp(const TopoDS_Shape& a, const TopoDS_Shape& b, BoolOp op)
{
    SolidResult result;
    if (a.IsNull() || b.IsNull()) {
        result.message = QStringLiteral("boolean needs two solids");
        return result;
    }
    TopoDS_Shape out;
    switch (op) {
    case BoolOp::Union: {
        BRepAlgoAPI_Fuse alg(a, b);
        if (alg.IsDone())
            out = alg.Shape();
        break;
    }
    case BoolOp::Subtract: {
        BRepAlgoAPI_Cut alg(a, b);
        if (alg.IsDone())
            out = alg.Shape();
        break;
    }
    case BoolOp::Intersect: {
        BRepAlgoAPI_Common alg(a, b);
        if (alg.IsDone())
            out = alg.Shape();
        break;
    }
    }
    if (out.IsNull()) {
        result.message = QStringLiteral("boolean operation failed");
        return result;
    }
    result.ok = true;
    result.shape = out;
    return result;
}

} // namespace solidops
} // namespace viki
