#include "SolidOps.h"

#include <algorithm>
#include <cmath>
#include <map>

#include <BRepAdaptor_CompCurve.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepOffsetAPI_MakePipe.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GC_MakeSegment.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_Surface.hxx>
#include <Standard_Failure.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Precision.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopoDS_Compound.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopExp.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "geom/GeomUtil.h"
#include "solid/SolidEntity.h"

namespace viki {
namespace solidops {
namespace {

gp_Pnt to3d(const Vec2d& p, const WorkPlane& plane)
{
    const gp_Vec x(plane.xDir);
    const gp_Vec y(plane.normal.Crossed(plane.xDir));
    return plane.origin.Translated(x * p.x + y * p.y);
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

namespace {

// Order a bag of open pieces into a single connected path (tolerance 1e-6),
// reversing pieces as needed. Returns false if they don't form one chain.
bool chainPath(std::vector<ProfilePiece> open, std::vector<ProfilePiece>& out)
{
    constexpr double tol = 1e-6;
    if (open.empty())
        return false;
    out.clear();
    out.push_back(open.front());
    open.erase(open.begin());
    bool grew = true;
    while (grew && !open.empty()) {
        grew = false;
        for (size_t i = 0; i < open.size(); ++i) {
            ProfilePiece p = open[i];
            if (nearEqual(out.back().b, p.a, tol)) {
                out.push_back(p);
            } else if (nearEqual(out.back().b, p.b, tol)) {
                std::swap(p.a, p.b);
                out.push_back(p);
            } else if (nearEqual(out.front().a, p.b, tol)) {
                out.insert(out.begin(), p);
            } else if (nearEqual(out.front().a, p.a, tol)) {
                std::swap(p.a, p.b);
                out.insert(out.begin(), p);
            } else {
                continue;
            }
            open.erase(open.begin() + long(i));
            grew = true;
            break;
        }
    }
    return open.empty();
}

// Build an OPEN wire from an ordered chain of pieces (does not close the loop).
TopoDS_Wire openWireFromChain(const std::vector<ProfilePiece>& chain,
                              const WorkPlane& plane)
{
    BRepBuilderAPI_MakeWire maker;
    for (const ProfilePiece& p : chain) {
        if (p.isArc) {
            const auto arc = GC_MakeArcOfCircle(to3d(p.a, plane), to3d(p.mid, plane),
                                                to3d(p.b, plane));
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

WireResult pathWireFromEntities(const Document& doc, const std::vector<EntityId>& ids,
                                const WorkPlane& plane)
{
    WireResult result;
    std::vector<std::vector<ProfilePiece>> loops; // closed entities (circle/closed pl)
    std::vector<ProfilePiece> open;
    for (const EntityId id : ids) {
        const Entity* e = doc.entity(id);
        if (!e) {
            result.message = QStringLiteral("entity %1 not found").arg(id);
            return result;
        }
        piecesFromEntity(*e, loops, open);
    }
    // A closed loop makes no sense as a sweep spine.
    for (auto& loop : loops)
        open.insert(open.end(), loop.begin(), loop.end());
    if (open.empty()) {
        result.message = QStringLiteral("no path geometry (need lines/arcs/polyline)");
        return result;
    }
    std::vector<ProfilePiece> chain;
    if (!chainPath(std::move(open), chain)) {
        result.message =
            QStringLiteral("path is not a single chain (connect the segments)");
        return result;
    }
    const TopoDS_Wire wire = openWireFromChain(chain, plane);
    if (wire.IsNull()) {
        result.message = QStringLiteral("failed to build the path wire");
        return result;
    }
    result.wires.push_back(wire);
    result.ok = true;
    return result;
}

namespace {

// Build the prism(s) for `wires`, extruding by `dir` from `base` (a point on the
// starting plane; the faces are moved there first so a symmetric extrude can
// start below the work plane). Multiple wires are fused into one solid.
SolidResult prismFromWires(const std::vector<TopoDS_Wire>& wires, const gp_Vec& base,
                           const gp_Vec& dir)
{
    SolidResult result;
    TopoDS_Shape acc;
    for (const TopoDS_Wire& wire : wires) {
        BRepBuilderAPI_MakeFace face(wire);
        if (!face.IsDone()) {
            result.message = QStringLiteral("profile wire does not bound a face");
            return result;
        }
        TopoDS_Shape faceShape = face.Face();
        if (base.Magnitude() > 1e-12) {
            gp_Trsf move;
            move.SetTranslation(base);
            faceShape = BRepBuilderAPI_Transform(faceShape, move, true).Shape();
        }
        BRepPrimAPI_MakePrism prism(faceShape, dir);
        if (prism.Shape().IsNull()) {
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

} // namespace

SolidResult extrudeWires(const std::vector<TopoDS_Wire>& wires, double height,
                         const WorkPlane& plane)
{
    SolidResult result;
    if (nearZero(height)) {
        result.message = QStringLiteral("height must be non-zero");
        return result;
    }
    const gp_Vec dir = gp_Vec(plane.normal) * height; // along the plane normal
    return prismFromWires(wires, gp_Vec(0, 0, 0), dir);
}

SolidResult extrudeWires(const std::vector<TopoDS_Wire>& wires, double height,
                         const WorkPlane& plane, ExtrudeMode mode,
                         const TopoDS_Shape& target)
{
    SolidResult result;
    if (nearZero(height)) {
        result.message = QStringLiteral("height must be non-zero");
        return result;
    }
    if ((mode == ExtrudeMode::Join || mode == ExtrudeMode::Cut) && target.IsNull()) {
        result.message = QStringLiteral("Join/Cut needs a target solid");
        return result;
    }

    const gp_Vec n(plane.normal);
    gp_Vec base(0, 0, 0);
    gp_Vec dir = n * height;
    if (mode == ExtrudeMode::Symmetric) {
        // Centre the prism on the work plane: start half below, run the full
        // height, so the solid spans -height/2 .. +height/2 about the plane.
        base = n * (-height / 2.0);
        dir = n * height;
    }

    const SolidResult prism = prismFromWires(wires, base, dir);
    if (!prism.ok)
        return prism;

    switch (mode) {
    case ExtrudeMode::NewBody:
    case ExtrudeMode::Symmetric:
        result.ok = true;
        result.shape = prism.shape;
        return result;
    case ExtrudeMode::Join:
        return booleanOp(target, prism.shape, BoolOp::Union);
    case ExtrudeMode::Cut:
        return booleanOp(target, prism.shape, BoolOp::Subtract);
    }
    result.message = QStringLiteral("unknown extrude mode");
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
    // Map the in-plane 2D axis direction to 3D through the work-plane frame.
    const gp_Vec ax3 = gp_Vec(plane.xDir) * dir.x +
                       gp_Vec(plane.normal.Crossed(plane.xDir)) * dir.y;
    const gp_Ax1 axis(to3d(axisA, plane), gp_Dir(ax3));
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

SolidResult sweepProfile(const std::vector<TopoDS_Wire>& profileWires,
                         const TopoDS_Wire& pathWire)
{
    SolidResult result;
    if (profileWires.empty()) {
        result.message = QStringLiteral("sweep needs a profile");
        return result;
    }
    if (pathWire.IsNull()) {
        result.message = QStringLiteral("sweep needs a path");
        return result;
    }
    // Path start point and start tangent (the spine's initial direction). The
    // profile is reoriented so its plane is perpendicular to this tangent and
    // its centre sits at the path start, so the sweep encloses a real volume
    // even when the profile was sketched coplanar with the path (the usual 2D
    // workflow). MakePipe itself keeps the profile rigid along the spine.
    gp_Pnt startPnt;
    gp_Vec startTan;
    try {
        BRepAdaptor_CompCurve spine(pathWire);
        const double u0 = spine.FirstParameter();
        spine.D1(u0, startPnt, startTan);
    } catch (const Standard_Failure& e) {
        result.message = QStringLiteral("bad sweep path: %1")
                             .arg(QString::fromUtf8(e.GetMessageString()));
        return result;
    }
    if (startTan.Magnitude() < 1e-9) {
        result.message = QStringLiteral("degenerate sweep path");
        return result;
    }
    const gp_Dir tangent(startTan);

    TopoDS_Shape acc;
    for (const TopoDS_Wire& wire : profileWires) {
        BRepBuilderAPI_MakeFace face(wire);
        if (!face.IsDone()) {
            result.message = QStringLiteral("profile wire does not bound a face");
            return result;
        }
        // Reorient the profile face: rotate its plane normal onto the path
        // tangent and translate its centroid onto the path start point.
        TopoDS_Shape profShape = face.Face();
        GProp_GProps sprops;
        BRepGProp::SurfaceProperties(profShape, sprops);
        const gp_Pnt centroid = sprops.CentreOfMass();
        gp_Dir profNormal(0, 0, 1);
        {
            const Handle(Geom_Surface) surf = BRep_Tool::Surface(TopoDS::Face(profShape));
            GeomLProp_SLProps sl(surf, 0.0, 0.0, 1, 1e-7);
            if (sl.IsNormalDefined())
                profNormal = sl.Normal();
        }
        gp_Trsf orient;
        const double dotv = profNormal.Dot(tangent);
        if (dotv < 1.0 - 1e-9) {
            gp_Dir rotAxis;
            if (dotv <= -1.0 + 1e-9) {
                // Antiparallel: pick any axis perpendicular to the normal.
                gp_Vec seed = std::fabs(profNormal.Z()) < 0.9 ? gp_Vec(0, 0, 1)
                                                              : gp_Vec(1, 0, 0);
                rotAxis = gp_Dir(gp_Vec(profNormal).Crossed(seed));
            } else {
                rotAxis = gp_Dir(gp_Vec(profNormal).Crossed(gp_Vec(tangent)));
            }
            const double ang = gp_Vec(profNormal).Angle(gp_Vec(tangent));
            orient.SetRotation(gp_Ax1(centroid, rotAxis), ang);
        }
        gp_Trsf move;
        move.SetTranslation(gp_Vec(centroid, startPnt));
        profShape = BRepBuilderAPI_Transform(profShape, move.Multiplied(orient), true)
                        .Shape();
        try {
            BRepOffsetAPI_MakePipe pipe(pathWire, TopoDS::Face(profShape));
            pipe.Build();
            if (pipe.Shape().IsNull()) {
                result.message = QStringLiteral("sweep failed");
                return result;
            }
            const TopoDS_Shape swept = pipe.Shape();
            if (acc.IsNull()) {
                acc = swept;
            } else {
                BRepAlgoAPI_Fuse fuse(acc, swept);
                if (!fuse.IsDone()) {
                    result.message = QStringLiteral("fusing swept profiles failed");
                    return result;
                }
                acc = fuse.Shape();
            }
        } catch (const Standard_Failure& e) {
            result.message = QStringLiteral("sweep failed: %1")
                                 .arg(QString::fromUtf8(e.GetMessageString()));
            return result;
        }
    }
    result.ok = true;
    result.shape = acc;
    return result;
}

SolidResult loftProfiles(const std::vector<TopoDS_Wire>& sections, bool solid)
{
    SolidResult result;
    if (sections.size() < 2) {
        result.message = QStringLiteral("loft needs at least two sections");
        return result;
    }
    try {
        BRepOffsetAPI_ThruSections gen(solid ? Standard_True : Standard_False);
        for (const TopoDS_Wire& wire : sections) {
            if (wire.IsNull()) {
                result.message = QStringLiteral("null loft section");
                return result;
            }
            gen.AddWire(wire);
        }
        gen.Build();
        if (gen.Shape().IsNull()) {
            result.message = QStringLiteral("loft failed");
            return result;
        }
        result.ok = true;
        result.shape = gen.Shape();
    } catch (const Standard_Failure& e) {
        result.message =
            QStringLiteral("loft failed: %1").arg(QString::fromUtf8(e.GetMessageString()));
    }
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
    // A boolean that leaves NO solid is a failure, not a success: committing
    // an empty compound made whole parts vanish (and looked like a bug you
    // could not undo). Cutting everything away is reported the same way.
    bool hasSolid = false;
    for (TopExp_Explorer e(out, TopAbs_SOLID); e.More(); e.Next()) {
        hasSolid = true;
        break;
    }
    if (!hasSolid) {
        result.message =
            QStringLiteral("boolean produced no solid (everything was cut "
                           "away or the shapes did not intersect)");
        return result;
    }
    result.ok = true;
    result.shape = out;
    return result;
}

std::vector<TopoDS_Shape> splitSolid(const TopoDS_Shape& solid,
                                     const TopoDS_Shape& tool)
{
    std::vector<TopoDS_Shape> pieces;
    if (solid.IsNull() || tool.IsNull())
        return pieces;
    try {
        BRepAlgoAPI_Splitter splitter;
        TopTools_ListOfShape args, tools;
        args.Append(solid);
        tools.Append(tool);
        splitter.SetArguments(args);
        splitter.SetTools(tools);
        splitter.Build();
        TopoDS_Shape out;
        try {
            out = splitter.Shape(); // force the build; don't trust IsDone()
        } catch (const Standard_Failure&) {
            out.Nullify();
        }
        if (out.IsNull())
            return pieces;
        for (TopExp_Explorer exp(out, TopAbs_SOLID); exp.More(); exp.Next())
            pieces.push_back(exp.Current());
    } catch (const Standard_Failure&) {
        pieces.clear();
    }
    return pieces;
}

std::vector<TopoDS_Shape> splitByPlane(const TopoDS_Shape& solid, const gp_Pln& pln)
{
    if (solid.IsNull())
        return {};
    Bnd_Box bb;
    BRepBndLib::Add(solid, bb);
    if (bb.IsVoid())
        return {};
    double xmin, ymin, zmin, xmax, ymax, zmax;
    bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const gp_Pnt centre((xmin + xmax) / 2.0, (ymin + ymax) / 2.0,
                        (zmin + zmax) / 2.0);
    // Half-size of the cutting face: the bbox diagonal plus margin guarantees
    // the bounded face covers the whole solid wherever the plane sits.
    const double half =
        gp_Pnt(xmin, ymin, zmin).Distance(gp_Pnt(xmax, ymax, zmax)) + 10.0;
    // Centre the face under the solid in the plane's own UV frame (the plane's
    // location can be far away, e.g. a principal plane at the origin).
    const gp_Ax3 pos = pln.Position();
    const gp_Vec toCentre(pos.Location(), centre);
    const double u0 = toCentre.Dot(gp_Vec(pos.XDirection()));
    const double v0 = toCentre.Dot(gp_Vec(pos.YDirection()));
    TopoDS_Shape face;
    try {
        face = BRepBuilderAPI_MakeFace(pln, u0 - half, u0 + half, v0 - half,
                                       v0 + half)
                   .Shape();
    } catch (const Standard_Failure&) {
        return {};
    }
    if (face.IsNull())
        return {};
    return splitSolid(solid, face);
}

namespace {

// Collect the first `n` edges of `solid` in TopExp order. n <= 0 or n beyond
// the edge count takes them all.
std::vector<TopoDS_Shape> firstNEdges(const TopoDS_Shape& solid, int n)
{
    std::vector<TopoDS_Shape> edges;
    for (TopExp_Explorer exp(solid, TopAbs_EDGE); exp.More(); exp.Next()) {
        if (n > 0 && static_cast<int>(edges.size()) >= n)
            break;
        edges.push_back(exp.Current());
    }
    return edges;
}

} // namespace

SolidResult filletEdges(const TopoDS_Shape& solid,
                        const std::vector<TopoDS_Shape>& edges, double radius)
{
    SolidResult result;
    if (solid.IsNull()) {
        result.message = QStringLiteral("fillet needs a target solid");
        return result;
    }
    if (radius <= 1e-9) {
        result.message = QStringLiteral("fillet radius must be positive");
        return result;
    }
    if (edges.empty()) {
        result.message = QStringLiteral("fillet needs at least one edge");
        return result;
    }
    try {
        BRepFilletAPI_MakeFillet op(solid);
        int added = 0;
        for (const TopoDS_Shape& e : edges) {
            if (e.IsNull() || e.ShapeType() != TopAbs_EDGE)
                continue;
            op.Add(radius, TopoDS::Edge(e));
            ++added;
        }
        if (added == 0) {
            result.message = QStringLiteral("no valid edges to fillet");
            return result;
        }
        op.Build();
        TopoDS_Shape s;
        try {
            s = op.Shape(); // don't trust IsDone(); force build + null-check
        } catch (const Standard_Failure&) {
            s.Nullify();
        }
        if (!s.IsNull()) {
            result.ok = true;
            result.shape = s;
            return result;
        }
    } catch (const Standard_Failure&) {
        // OCCT throws on infeasible radii; fall through to the message.
    }
    result.message = QStringLiteral("fillet failed — radius too large for this geometry?");
    return result;
}

SolidResult chamferEdges(const TopoDS_Shape& solid,
                         const std::vector<TopoDS_Shape>& edges, double distance)
{
    SolidResult result;
    if (solid.IsNull()) {
        result.message = QStringLiteral("chamfer needs a target solid");
        return result;
    }
    if (distance <= 1e-9) {
        result.message = QStringLiteral("chamfer distance must be positive");
        return result;
    }
    if (edges.empty()) {
        result.message = QStringLiteral("chamfer needs at least one edge");
        return result;
    }
    try {
        BRepFilletAPI_MakeChamfer op(solid);
        int added = 0;
        for (const TopoDS_Shape& e : edges) {
            if (e.IsNull() || e.ShapeType() != TopAbs_EDGE)
                continue;
            op.Add(distance, TopoDS::Edge(e));
            ++added;
        }
        if (added == 0) {
            result.message = QStringLiteral("no valid edges to chamfer");
            return result;
        }
        op.Build();
        TopoDS_Shape s;
        try {
            s = op.Shape(); // don't trust IsDone(); force build + null-check
        } catch (const Standard_Failure&) {
            s.Nullify();
        }
        if (!s.IsNull()) {
            result.ok = true;
            result.shape = s;
            return result;
        }
    } catch (const Standard_Failure&) {
        // OCCT throws on infeasible distances; fall through to the message.
    }
    result.message =
        QStringLiteral("chamfer failed — distance too large for this geometry?");
    return result;
}

SolidResult filletFirstNEdges(const TopoDS_Shape& solid, int n, double radius)
{
    if (solid.IsNull()) {
        SolidResult r;
        r.message = QStringLiteral("fillet needs a target solid");
        return r;
    }
    return filletEdges(solid, firstNEdges(solid, n), radius);
}

SolidResult chamferFirstNEdges(const TopoDS_Shape& solid, int n, double distance)
{
    if (solid.IsNull()) {
        SolidResult r;
        r.message = QStringLiteral("chamfer needs a target solid");
        return r;
    }
    return chamferEdges(solid, firstNEdges(solid, n), distance);
}

// Outward unit normal at the centre of a planar-ish face (orientation-aware).
// Returns false if the normal is degenerate.
static bool faceOutwardNormal(const TopoDS_Face& f, gp_Dir& outNormal)
{
    BRepAdaptor_Surface surf(f);
    const Standard_Real u = 0.5 * (surf.FirstUParameter() + surf.LastUParameter());
    const Standard_Real v = 0.5 * (surf.FirstVParameter() + surf.LastVParameter());
    gp_Pnt p;
    gp_Vec du, dv;
    surf.D1(u, v, p, du, dv);
    gp_Vec n = du.Crossed(dv);
    if (n.Magnitude() < 1e-12)
        return false;
    n.Normalize();
    if (f.Orientation() == TopAbs_REVERSED)
        n.Reverse();
    outNormal = gp_Dir(n);
    return true;
}

SolidResult draftFaces(const TopoDS_Shape& solid,
                       const std::vector<TopoDS_Shape>& faces, const gp_Dir& pullDir,
                       const gp_Pln& neutralPlane, double angleDeg)
{
    SolidResult result;
    if (solid.IsNull()) {
        result.message = QStringLiteral("draft needs a target solid");
        return result;
    }
    if (faces.empty()) {
        result.message = QStringLiteral("draft needs at least one face");
        return result;
    }
    const double angleRad = angleDeg * M_PI / 180.0;
    try {
        BRepOffsetAPI_DraftAngle op(solid);
        int added = 0;
        for (const TopoDS_Shape& fShape : faces) {
            if (fShape.IsNull() || fShape.ShapeType() != TopAbs_FACE)
                continue;
            const TopoDS_Face face = TopoDS::Face(fShape);
            op.Add(face, pullDir, angleRad, neutralPlane);
            if (!op.AddDone()) {
                // OCCT rejected this face (e.g. perpendicular to the pull dir);
                // remove it and skip so the rest can still be drafted.
                op.Remove(face);
                continue;
            }
            ++added;
        }
        if (added == 0) {
            result.message = QStringLiteral("no valid faces to draft");
            return result;
        }
        op.Build();
        TopoDS_Shape s;
        try {
            s = op.Shape(); // don't trust IsDone(); force build + null-check
        } catch (const Standard_Failure&) {
            s.Nullify();
        }
        if (!s.IsNull()) {
            result.ok = true;
            result.shape = s;
            return result;
        }
    } catch (const Standard_Failure&) {
        // OCCT throws on infeasible angles; fall through to the message.
    }
    result.message =
        QStringLiteral("draft failed — angle too large for this geometry?");
    return result;
}

SolidResult draftBoxSides(const TopoDS_Shape& solid, const gp_Dir& pullDir,
                          const gp_Pln& neutralPlane, double angleDeg)
{
    SolidResult result;
    if (solid.IsNull()) {
        result.message = QStringLiteral("draft needs a target solid");
        return result;
    }
    // Side faces = those whose outward normal is perpendicular to the pull
    // direction (top/bottom caps are parallel/antiparallel and get skipped).
    std::vector<TopoDS_Shape> sideFaces;
    for (TopExp_Explorer exp(solid, TopAbs_FACE); exp.More(); exp.Next()) {
        const TopoDS_Face f = TopoDS::Face(exp.Current());
        gp_Dir n;
        if (!faceOutwardNormal(f, n))
            continue;
        if (std::abs(n.Dot(pullDir)) < 1e-4)
            sideFaces.push_back(f);
    }
    if (sideFaces.empty()) {
        result.message = QStringLiteral("no side faces perpendicular to the pull direction");
        return result;
    }
    return draftFaces(solid, sideFaces, pullDir, neutralPlane, angleDeg);
}

SolidResult shellSolid(const TopoDS_Shape& solid, double thickness,
                       const TopoDS_Shape& openFace)
{
    std::vector<TopoDS_Shape> faces;
    if (!openFace.IsNull())
        faces.push_back(openFace);
    return shellSolid(solid, thickness, faces);
}

SolidResult shellSolid(const TopoDS_Shape& solid, double thickness,
                       const std::vector<TopoDS_Shape>& openFaces)
{
    SolidResult result;
    if (solid.IsNull()) {
        result.message = QStringLiteral("shell needs a target solid");
        return result;
    }
    if (thickness <= 1e-9) {
        result.message = QStringLiteral("shell thickness must be positive");
        return result;
    }

    // Faces to open (remove). Empty = closed shell all around.
    TopTools_ListOfShape toRemove;
    for (const TopoDS_Shape& f : openFaces) {
        if (f.IsNull() || f.ShapeType() != TopAbs_FACE) {
            result.message = QStringLiteral("shell open-face must be a face");
            return result;
        }
        toRemove.Append(f);
    }

    // Negative offset hollows inward, leaving a wall of |thickness|. The Join
    // variant honours the face-removal list; it also produces a fully closed
    // hollow when the list is empty. Don't trust IsDone() (unreliable before
    // the shape is realised) — force .Shape() and null-check.
    try {
        BRepOffsetAPI_MakeThickSolid op;
        op.MakeThickSolidByJoin(solid, toRemove, -std::fabs(thickness), 1.0e-3);
        op.Build();
        TopoDS_Shape s;
        try {
            s = op.Shape(); // don't trust IsDone(); force build + null-check
        } catch (const Standard_Failure&) {
            s.Nullify();
        }
        if (!s.IsNull()) {
            if (toRemove.IsEmpty()) {
                // With no face removed, MakeThickSolidByJoin returns the inner
                // offset solid (the cavity), not the wall. The closed shell is
                // then `solid` minus that cavity, leaving the wall all around.
                const auto cut = booleanOp(solid, s, BoolOp::Subtract);
                if (cut.ok && !cut.shape.IsNull()) {
                    result.ok = true;
                    result.shape = cut.shape;
                    return result;
                }
            } else {
                // A removed face produces the proper open shell directly.
                result.ok = true;
                result.shape = s;
                return result;
            }
        }
    } catch (const Standard_Failure&) {
        // OCCT throws on infeasible thickness; fall through to the message.
    }
    result.message = QStringLiteral("shell failed — thickness too large for "
                                    "this geometry?");
    return result;
}

SolidResult pushPullFace(const TopoDS_Shape& solid, const TopoDS_Shape& face,
                         double distance)
{
    SolidResult result;
    if (solid.IsNull() || face.IsNull() || face.ShapeType() != TopAbs_FACE) {
        result.message = QStringLiteral("push/pull needs a solid and one of its faces");
        return result;
    }
    if (std::fabs(distance) < 1e-9) {
        result.message = QStringLiteral("push/pull distance is zero");
        return result;
    }
    const TopoDS_Face f = TopoDS::Face(face);

    // Only PLANAR faces can be push/pulled (v1): prisming a curved face and
    // fusing it back produces an empty/garbage boolean that used to be
    // reported as success — and the whole part "disappeared".
    BRepAdaptor_Surface surf(f);
    if (surf.GetType() != GeomAbs_Plane) {
        result.message = QStringLiteral(
            "push/pull needs a PLANAR face (this one is curved) — use SHELL, "
            "DRAFT or a sketch + EXTRUDE Cut instead");
        return result;
    }
    const Standard_Real u = 0.5 * (surf.FirstUParameter() + surf.LastUParameter());
    const Standard_Real v = 0.5 * (surf.FirstVParameter() + surf.LastVParameter());
    gp_Pnt p;
    gp_Vec du, dv;
    surf.D1(u, v, p, du, dv);
    gp_Vec n = du.Crossed(dv);
    if (n.Magnitude() < 1e-12) {
        result.message = QStringLiteral("cannot find the face normal");
        return result;
    }
    n.Normalize();
    if (f.Orientation() == TopAbs_REVERSED)
        n.Reverse();

    BRepPrimAPI_MakePrism prism(f, n * distance);
    if (!prism.IsDone()) {
        result.message = QStringLiteral("extruding the face failed");
        return result;
    }
    // distance > 0 grows the material (boss), < 0 removes it (pocket).
    return booleanOp(solid, prism.Shape(),
                     distance > 0 ? BoolOp::Union : BoolOp::Subtract);
}

SolidResult makeHole(const TopoDS_Shape& solid, const WorkPlane& plane,
                     const Vec2d& center, double diameter, double depth,
                     bool through)
{
    SolidResult result;
    if (solid.IsNull()) {
        result.message = QStringLiteral("hole needs a target solid");
        return result;
    }
    if (diameter <= 1e-9) {
        result.message = QStringLiteral("hole diameter must be positive");
        return result;
    }
    if (!through && depth <= 1e-9) {
        result.message = QStringLiteral("hole depth must be positive");
        return result;
    }

    const gp_Pnt c3 = to3d(center, plane);   // hole centre, on the work plane
    const gp_Dir n = plane.normal;            // bore axis

    // The bore goes INTO the material, i.e. opposite the plane normal (the
    // normal points out of the sketch face, like EXTRUDE's positive direction).
    double length = depth;
    gp_Pnt base = c3;
    if (through) {
        // Span the solid's full extent along the normal, with generous margin,
        // so the cylinder pierces clean through regardless of where the work
        // plane sits relative to the solid.
        Bnd_Box bb;
        BRepBndLib::Add(solid, bb);
        if (bb.IsVoid()) {
            result.message = QStringLiteral("target solid is empty");
            return result;
        }
        double xmin, ymin, zmin, xmax, ymax, zmax;
        bb.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        const gp_Vec axis(n);
        // Project the 8 bbox corners onto the axis to find the extent.
        const double xs[2] = {xmin, xmax};
        const double ys[2] = {ymin, ymax};
        const double zs[2] = {zmin, zmax};
        double lo = 1e300, hi = -1e300;
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j)
                for (int k = 0; k < 2; ++k) {
                    const gp_Vec corner(xs[i], ys[j], zs[k]);
                    const double t = corner.Dot(axis);
                    lo = std::min(lo, t);
                    hi = std::max(hi, t);
                }
        const double centreT = gp_Vec(c3.X(), c3.Y(), c3.Z()).Dot(axis);
        const double span = hi - lo;
        const double margin = span * 0.1 + diameter + 1.0;
        // Start below the far side, run all the way past the near side.
        const double startT = lo - margin;
        length = (hi + margin) - startT;
        base = c3.Translated(axis * (startT - centreT));
    } else {
        // Blind hole: cylinder base at the plane, boring inward (-normal).
        base = c3;
        length = depth;
    }

    const gp_Dir boreDir = through ? n : gp_Dir(gp_Vec(n).Reversed());
    const gp_Ax2 ax(base, boreDir);
    // Note: BRepPrimAPI_MakeCylinder::IsDone() is unreliable before the shape is
    // built, so force the build via Shape() and check for a null result instead.
    const TopoDS_Shape tool = BRepPrimAPI_MakeCylinder(ax, diameter / 2.0, length).Shape();
    if (tool.IsNull()) {
        result.message = QStringLiteral("building the hole cylinder failed");
        return result;
    }
    return booleanOp(solid, tool, BoolOp::Subtract);
}

std::optional<WorkPlane> planeFromFace(const TopoDS_Shape& face)
{
    if (face.IsNull() || face.ShapeType() != TopAbs_FACE)
        return std::nullopt;
    BRepAdaptor_Surface surf(TopoDS::Face(face));
    if (surf.GetType() != GeomAbs_Plane)
        return std::nullopt; // can only sketch on a planar face
    const gp_Pln pln = surf.Plane();
    WorkPlane wp;
    wp.origin = pln.Location();
    wp.normal = pln.Axis().Direction();
    wp.xDir = pln.XAxis().Direction();
    // Face orientation flips the surface normal; keep the outward sense.
    if (TopoDS::Face(face).Orientation() == TopAbs_REVERSED)
        wp.normal.Reverse();
    return wp;
}

Vec2d projectToPlane2d(const gp_Pnt& p, const WorkPlane& plane)
{
    const gp_Vec d(plane.origin, p);
    const gp_Vec x(plane.xDir);
    const gp_Vec y(gp_Vec(plane.normal).Crossed(gp_Vec(plane.xDir)));
    return Vec2d{d.Dot(x), d.Dot(y)};
}

gp_Pnt planePoint3d(const Vec2d& uv, const WorkPlane& plane)
{
    return to3d(uv, plane);
}

std::optional<gp_Trsf> mateTransform(const TopoDS_Shape& faceA,
                                     const TopoDS_Shape& faceB)
{
    // Both faces must be planar; reuse planeFromFace to get their outward
    // (orientation-respecting) frames.
    const auto pa = planeFromFace(faceA);
    const auto pb = planeFromFace(faceB);
    if (!pa || !pb)
        return std::nullopt;

    const gp_Dir nA = pa->normal;
    const gp_Dir nB = pb->normal;

    // Use the face centroids as the frame origins so that after the mate the
    // two faces are not only coplanar but overlapping (their centres coincide),
    // which is the intuitive "snap flat, centred" behaviour.
    GProp_GProps gpA, gpB;
    BRepGProp::SurfaceProperties(TopoDS::Face(faceA), gpA);
    BRepGProp::SurfaceProperties(TopoDS::Face(faceB), gpB);
    const gp_Pnt cA = gpA.CentreOfMass();
    const gp_Pnt cB = gpB.CentreOfMass();

    // Source frame: faceA's centroid, its outward normal as Z, its plane xDir.
    // Target frame: faceB's centroid, with Z along faceB's INWARD direction
    // (-nB) so that after the move faceA's outward normal ends up opposed to
    // faceB's outward normal (the two faces face each other) while the two
    // planes are coincident. xDir/normal come from the planar surface and are
    // guaranteed perpendicular, a valid gp_Ax3 X reference.
    const gp_Ax3 src(cA, nA, pa->xDir);
    const gp_Ax3 dst(cB, gp_Dir(gp_Vec(nB).Reversed()), pb->xDir);

    gp_Trsf t;
    // Maps the src frame onto the dst frame: any point expressed in src's
    // coordinates lands at the same coordinates in dst. This rigidly moves the
    // moving solid so faceA snaps flat against faceB.
    t.SetDisplacement(src.Ax2(), dst.Ax2());
    return t;
}

TopoDS_Shape sectionWires(const TopoDS_Shape& solid, const gp_Pln& pln)
{
    if (solid.IsNull())
        return TopoDS_Shape();
    try {
        BRepAlgoAPI_Section sec(solid, pln, Standard_False);
        sec.ComputePCurveOn1(Standard_True); // pcurves on the solid's faces
        sec.Approximation(Standard_True);     // approximate the section curves
        sec.Build();
        if (!sec.IsDone())
            return TopoDS_Shape();
        const TopoDS_Shape edges = sec.Shape();
        if (edges.IsNull())
            return TopoDS_Shape();
        // Reassemble the loose section edges into wires so callers get closed
        // profiles rather than a bag of edges.
        Handle(TopTools_HSequenceOfShape) edgeSeq = new TopTools_HSequenceOfShape;
        for (TopExp_Explorer ex(edges, TopAbs_EDGE); ex.More(); ex.Next())
            edgeSeq->Append(ex.Current());
        if (edgeSeq->IsEmpty())
            return edges; // nothing to connect (e.g. a point touch)
        Handle(TopTools_HSequenceOfShape) wireSeq;
        ShapeAnalysis_FreeBounds::ConnectEdgesToWires(edgeSeq, Precision::Confusion(),
                                                      Standard_False, wireSeq);
        BRep_Builder builder;
        TopoDS_Compound comp;
        builder.MakeCompound(comp);
        if (!wireSeq.IsNull())
            for (Standard_Integer i = 1; i <= wireSeq->Length(); ++i)
                builder.Add(comp, wireSeq->Value(i));
        return comp;
    } catch (const Standard_Failure&) {
        return TopoDS_Shape();
    }
}

double sectionArea(const TopoDS_Shape& solid, const gp_Pln& pln)
{
    const TopoDS_Shape wires = sectionWires(solid, pln);
    if (wires.IsNull())
        return 0.0;
    try {
        // Face every closed section wire on the cutting plane and sum the areas.
        double total = 0.0;
        for (TopExp_Explorer ex(wires, TopAbs_WIRE); ex.More(); ex.Next()) {
            const TopoDS_Wire w = TopoDS::Wire(ex.Current());
            if (!w.Closed())
                continue;
            BRepBuilderAPI_MakeFace mkFace(pln, w, Standard_True);
            if (!mkFace.IsDone())
                continue;
            const TopoDS_Face f = mkFace.Face();
            if (f.IsNull())
                continue;
            GProp_GProps props;
            BRepGProp::SurfaceProperties(f, props);
            total += std::fabs(props.Mass());
        }
        return total;
    } catch (const Standard_Failure&) {
        return 0.0;
    }
}

double minDistance(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    if (a.IsNull() || b.IsNull())
        return -1.0;
    try {
        BRepExtrema_DistShapeShape ext(a, b);
        if (!ext.IsDone() || ext.NbSolution() < 1)
            return -1.0;
        return ext.Value();
    } catch (const Standard_Failure&) {
        return -1.0;
    }
}

double interferenceVolume(const TopoDS_Shape& a, const TopoDS_Shape& b)
{
    if (a.IsNull() || b.IsNull())
        return 0.0;
    try {
        BRepAlgoAPI_Common alg(a, b);
        if (!alg.IsDone())
            return 0.0;
        const TopoDS_Shape common = alg.Shape();
        if (common.IsNull())
            return 0.0;
        // A face/edge-only touch produces a common shape with no solid volume;
        // VolumeProperties then integrates to ~0, which is exactly what we want.
        GProp_GProps props;
        BRepGProp::VolumeProperties(common, props);
        const double v = props.Mass();
        return v > 0.0 ? v : 0.0;
    } catch (const Standard_Failure&) {
        return 0.0;
    }
}

std::vector<Interference> checkAllInterferences(const Document& doc, double minVolume)
{
    // Collect every solid, cheaply reject non-overlapping pairs via their
    // OCCT bounding boxes before paying for a boolean common.
    struct Item {
        EntityId id;
        const TopoDS_Shape* shape;
        Bnd_Box box;
    };
    std::vector<Item> items;
    for (const EntityId id : doc.drawOrder()) {
        const Entity* e = doc.entity(id);
        const auto* s = dynamic_cast<const SolidEntity*>(e);
        if (!s || s->shape().IsNull())
            continue;
        Bnd_Box box;
        BRepBndLib::Add(s->shape(), box);
        items.push_back({id, &s->shape(), box});
    }

    std::vector<Interference> out;
    for (size_t i = 0; i < items.size(); ++i) {
        for (size_t j = i + 1; j < items.size(); ++j) {
            if (items[i].box.IsOut(items[j].box))
                continue; // boxes disjoint → cannot overlap
            const double v = interferenceVolume(*items[i].shape, *items[j].shape);
            if (v > minVolume)
                out.push_back({items[i].id, items[j].id, v});
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Interference& p, const Interference& q) {
                  return p.volume > q.volume;
              });
    return out;
}

std::vector<std::vector<Vec2d>> faceOutline2d(const TopoDS_Shape& face,
                                              const WorkPlane& plane)
{
    std::vector<std::vector<Vec2d>> loops;
    if (face.IsNull() || face.ShapeType() != TopAbs_FACE)
        return loops;
    const gp_Vec xv(plane.xDir);
    const gp_Vec yv(plane.normal.Crossed(plane.xDir));
    const auto to2d = [&](const gp_Pnt& p) {
        const gp_Vec d(plane.origin, p);
        return Vec2d{d.Dot(xv), d.Dot(yv)};
    };
    for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(ex.Current());
        BRepAdaptor_Curve curve(edge);
        const double t0 = curve.FirstParameter();
        const double t1 = curve.LastParameter();
        // A line needs 2 points; curves are sampled finely.
        const int n = curve.GetType() == GeomAbs_Line ? 1 : 48;
        std::vector<Vec2d> poly;
        poly.reserve(n + 1);
        for (int i = 0; i <= n; ++i)
            poly.push_back(to2d(curve.Value(t0 + (t1 - t0) * i / n)));
        loops.push_back(std::move(poly));
    }
    return loops;
}

std::vector<SnapPoint> faceSnapPoints2d(const TopoDS_Shape& face,
                                        const WorkPlane& plane)
{
    std::vector<SnapPoint> pts;
    if (face.IsNull() || face.ShapeType() != TopAbs_FACE)
        return pts;
    const gp_Vec xv(plane.xDir);
    const gp_Vec yv(plane.normal.Crossed(plane.xDir));
    const auto to2d = [&](const gp_Pnt& p) {
        const gp_Vec d(plane.origin, p);
        return Vec2d{d.Dot(xv), d.Dot(yv)};
    };
    // Vertices → Endpoint targets (deduplicated: shared edge vertices appear once).
    TopTools_IndexedMapOfShape verts;
    TopExp::MapShapes(face, TopAbs_VERTEX, verts);
    for (int i = 1; i <= verts.Extent(); ++i) {
        const TopoDS_Vertex v = TopoDS::Vertex(verts(i));
        pts.push_back({to2d(BRep_Tool::Pnt(v)), SnapKind::Endpoint});
    }
    // Circular / elliptical edges → Center targets.
    for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
        BRepAdaptor_Curve curve(TopoDS::Edge(ex.Current()));
        if (curve.GetType() == GeomAbs_Circle)
            pts.push_back({to2d(curve.Circle().Location()), SnapKind::Center});
        else if (curve.GetType() == GeomAbs_Ellipse)
            pts.push_back({to2d(curve.Ellipse().Location()), SnapKind::Center});
    }
    return pts;
}

std::vector<gp_Trsf> patternRect(int nx, int ny, int nz, double dx, double dy,
                                 double dz)
{
    nx = std::max(nx, 1);
    ny = std::max(ny, 1);
    nz = std::max(nz, 1);
    std::vector<gp_Trsf> out;
    out.reserve(static_cast<size_t>(nx) * ny * nz);
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                gp_Trsf t;
                t.SetTranslation(gp_Vec(i * dx, j * dy, k * dz));
                out.push_back(t);
            }
    return out;
}

std::vector<gp_Trsf> patternPolar(int count, const gp_Dir& axis,
                                  const gp_Pnt& center, double totalAngleDeg)
{
    count = std::max(count, 1);
    std::vector<gp_Trsf> out;
    out.reserve(static_cast<size_t>(count));
    // Full turn: don't duplicate the endpoint, so divide by count. A partial
    // sweep spans endpoints, so divide by (count-1).
    const bool fullTurn = std::abs(std::abs(totalAngleDeg) - 360.0) < 1e-9;
    const double denom = (fullTurn || count <= 1)
                             ? static_cast<double>(count)
                             : static_cast<double>(count - 1);
    const double stepRad = (totalAngleDeg * M_PI / 180.0) / denom;
    const gp_Ax1 ax(center, axis);
    for (int i = 0; i < count; ++i) {
        gp_Trsf t;
        t.SetRotation(ax, i * stepRad);
        out.push_back(t);
    }
    return out;
}

} // namespace solidops

WorkPlane& documentWorkplane(const Document& doc)
{
    static std::map<const Document*, WorkPlane> planes;
    return planes[&doc];
}
} // namespace viki
