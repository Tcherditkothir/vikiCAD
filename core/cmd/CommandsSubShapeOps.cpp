#include "CommandProcessor.h"

#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"
#include "solid/SubShape.h"

#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <cmath>

// Headless twins of three GUI-only direct-modeling ops, addressed through the
// deterministic sub-shape indices that INSPECT prints (TopExp order, 0-based):
//
//   PUSHPULL <distance> <faceIndex> <solidId>
//       solidops::pushPullFace on that face — +distance is a boss, -distance
//       a pocket. Curved faces are refused with the PLANAR message from the
//       op itself (the "part vanished" guard).
//   SHELLOPEN <thickness> <solidId> <faceIndex> [faceIndex...]
//       multi-face shellSolid — hollow the solid, removing every listed face
//       (open top+bottom of a box = a square tube). The id and the indices
//       arrive in ONE EntitySet gulp; the first value is the solid id.
//   SPLITFACE <toolSolidId> <toolFaceIndex> <targetSolidId>
//       splitByFaceExtended with that face (planar OR curved, e.g. a cylinder
//       wall) as the cutting tool. The target is replaced by the pieces in one
//       transaction; every piece inherits layer/color/component/transparency
//       (the SPLIT command's replacement block, verbatim).
//   FILLETEDGES <radius> <solidId> <edgeIndex> [edgeIndex...]
//   CHAMFEREDGES <distance> <solidId> <edgeIndex> [edgeIndex...]
//       solidops::filletEdges / chamferEdges on the edgeAt() edges — the
//       per-edge twins of FILLET3D/CHAMFER3D (which take ALL edges).
//   MATE <movingSolidId> <movingFaceIndex> <fixedSolidId> <fixedFaceIndex>
//       solidops::mateTransform between the two PLANAR faces, applied to the
//       moving solid with SolidEntity::applyTrsf: its face snaps flat and
//       centred onto the fixed face, outward normals opposed.
//   DRAFT <angleDeg> <solidId> <faceIndex> [faceIndex...]
//       solidops::draftFaces with pullDir = +Z and the neutral plane = the XY
//       plane at the solid's zMin (the bottom of its bounding box). That
//       default suits the common case — a part extruded up from the work
//       plane, drafted for a +Z mold pull about its base; agents needing
//       another setup can WORKPLANE + reorient first.
//
// Grammar note: numeric params come BEFORE entity selection where possible
// (the greedy EntitySet gulp), and index-after-id groups ride inside a single
// EntitySet whose values are interpreted positionally.

namespace viki {
namespace {

// The SPLIT command's clone-with-fields helper: a piece inherits the source
// solid's layer, color, component and transparency.
std::unique_ptr<SolidEntity> solidLike(const TopoDS_Shape& shape,
                                       int64_t layerId, const ColorSpec& color,
                                       const QString& component,
                                       double transparency)
{
    auto e = std::make_unique<SolidEntity>(shape);
    e->setLayerId(layerId);
    e->setColor(color);
    e->component = component;
    e->transparency = transparency;
    return e;
}

// The face `index` of `solid`'s shape, with agent-friendly diagnostics.
TopoDS_Shape faceOrReport(CommandContext& ctx, const SolidEntity& solid,
                          EntityId id, int index)
{
    const TopoDS_Shape face = subshape::faceAt(solid.shape(), index);
    if (face.IsNull())
        ctx.info(QStringLiteral("solid %1 has no face %2 (it has %3 faces, "
                                "indices 0..%4 — see INSPECT)")
                     .arg(id)
                     .arg(index)
                     .arg(subshape::faceCount(solid.shape()))
                     .arg(subshape::faceCount(solid.shape()) - 1));
    return face;
}

// The edge `index` of `solid`'s shape, with agent-friendly diagnostics.
TopoDS_Shape edgeOrReport(CommandContext& ctx, const SolidEntity& solid,
                          EntityId id, int index)
{
    const TopoDS_Shape edge = subshape::edgeAt(solid.shape(), index);
    if (edge.IsNull())
        ctx.info(QStringLiteral("solid %1 has no edge %2 (it has %3 edges, "
                                "indices 0..%4 — see INSPECT)")
                     .arg(id)
                     .arg(index)
                     .arg(subshape::edgeCount(solid.shape()))
                     .arg(subshape::edgeCount(solid.shape()) - 1));
    return edge;
}

// PUSHPULL <distance> <faceIndex> <solidId>
class PushPullCommand : public Command {
public:
    const char* name() const override { return "PUSHPULL"; }

    Step start(CommandContext&) override
    {
        // Numeric params first (the EntitySet gulp would eat them).
        return Step::cont(InputKind::Number,
                          QStringLiteral("Distance (+boss / -pocket):"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_st) {
        case St::Dist:
            if (v.kind != InputValue::Kind::Number)
                return Step::cancelled();
            if (std::abs(v.number) <= kGeomTol) {
                ctx.info(QStringLiteral("distance must be non-zero"));
                return Step::cancelled();
            }
            m_dist = v.number;
            m_st = St::Face;
            return Step::cont(InputKind::Number,
                              QStringLiteral("Face index (see INSPECT):"));
        case St::Face:
            if (v.kind != InputValue::Kind::Number)
                return Step::cancelled();
            if (v.number < 0 || std::floor(v.number) != v.number) {
                ctx.info(QStringLiteral(
                    "face index must be a non-negative integer"));
                return Step::cancelled();
            }
            m_face = int(v.number);
            m_st = St::Solid;
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select solid (id):"));
        case St::Solid:
            return apply(ctx, v);
        }
        return Step::cancelled();
    }

private:
    Step apply(CommandContext& ctx, const InputValue& v)
    {
        EntityId id = kInvalidEntityId;
        if (v.kind == InputValue::Kind::EntitySet && v.entitySet.size() == 1)
            id = v.entitySet[0];
        else if (v.kind == InputValue::Kind::EntityRef)
            id = v.entityRef;
        if (id == kInvalidEntityId)
            return Step::cancelled();
        auto* solid = dynamic_cast<SolidEntity*>(ctx.doc().entity(id));
        if (!solid) {
            ctx.info(QStringLiteral("that id is not a solid"));
            return Step::cancelled();
        }
        const TopoDS_Shape face = faceOrReport(ctx, *solid, id, m_face);
        if (face.IsNull())
            return Step::cancelled();

        const auto out = solidops::pushPullFace(solid->shape(), face, m_dist);
        if (!out.ok || out.shape.IsNull()) {
            // The planar-face rejection (and every other guard) flows through.
            ctx.info(out.message.isEmpty()
                         ? QStringLiteral("push/pull failed")
                         : QStringLiteral("push/pull: %1").arg(out.message));
            return Step::cancelled();
        }
        ctx.doc().beginTransaction(QLatin1String(name()));
        auto* mut = static_cast<SolidEntity*>(ctx.doc().beginModify(id));
        mut->setShape(out.shape);
        ctx.doc().endModify(id);
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("push/pull face %1 of solid %2 by %3 mm")
                     .arg(m_face)
                     .arg(id)
                     .arg(m_dist));
        return Step::done();
    }

    enum class St { Dist, Face, Solid };
    St m_st = St::Dist;
    double m_dist = 0.0;
    int m_face = -1;
};

// SHELLOPEN <thickness> <solidId> <faceIndex> [faceIndex...]
class ShellOpenCommand : public Command {
public:
    const char* name() const override { return "SHELLOPEN"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Distance,
                          QStringLiteral("Wall thickness:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (m_thickness <= 0) {
            if (v.kind != InputValue::Kind::Number || v.number <= kGeomTol)
                return Step::cancelled();
            m_thickness = v.number;
            return Step::cont(
                InputKind::EntitySet,
                QStringLiteral("Solid id then open face index(es):"));
        }
        // Id + indices stage: one positional EntitySet gulp — values[0] is
        // the solid id, the rest are face indices. More lines may add more
        // indices; we build as soon as we have id + at least one index.
        if (v.kind == InputValue::Kind::EntitySet)
            m_values.insert(m_values.end(), v.entitySet.begin(),
                            v.entitySet.end());
        else if (v.kind == InputValue::Kind::EntityRef)
            m_values.push_back(v.entityRef);
        else {
            if (v.kind == InputValue::Kind::Finish)
                ctx.info(QStringLiteral(
                    "SHELLOPEN needs a solid id and at least one face index"));
            return Step::cancelled();
        }
        if (m_values.size() >= 2)
            return apply(ctx);
        return Step::cont(InputKind::EntitySet,
                          QStringLiteral("Open face index(es):"));
    }

private:
    Step apply(CommandContext& ctx)
    {
        const EntityId id = m_values[0];
        auto* solid = dynamic_cast<SolidEntity*>(ctx.doc().entity(id));
        if (!solid) {
            ctx.info(QStringLiteral("that id is not a solid"));
            return Step::cancelled();
        }
        std::vector<TopoDS_Shape> open;
        for (size_t i = 1; i < m_values.size(); ++i) {
            const TopoDS_Shape face =
                faceOrReport(ctx, *solid, id, int(m_values[i]));
            if (face.IsNull())
                return Step::cancelled();
            open.push_back(face);
        }
        const auto out =
            solidops::shellSolid(solid->shape(), m_thickness, open);
        if (!out.ok || out.shape.IsNull()) {
            ctx.info(out.message.isEmpty()
                         ? QStringLiteral("shell failed")
                         : QStringLiteral("shell: %1").arg(out.message));
            return Step::cancelled();
        }
        ctx.doc().beginTransaction(QLatin1String(name()));
        auto* mut = static_cast<SolidEntity*>(ctx.doc().beginModify(id));
        mut->setShape(out.shape);
        ctx.doc().endModify(id);
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral(
                     "shelled solid %1 to %2 mm wall, %3 face(s) open")
                     .arg(id)
                     .arg(m_thickness)
                     .arg(open.size()));
        return Step::done();
    }

    double m_thickness = 0.0;
    std::vector<EntityId> m_values; // [solidId, faceIndex...]
};

// SPLITFACE <toolSolidId> <toolFaceIndex> <targetSolidId>
class SplitFaceCommand : public Command {
public:
    const char* name() const override { return "SPLITFACE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(
            InputKind::EntitySet,
            QStringLiteral("Tool solid id, tool face index, target solid id:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (v.kind == InputValue::Kind::EntitySet)
            m_values.insert(m_values.end(), v.entitySet.begin(),
                            v.entitySet.end());
        else if (v.kind == InputValue::Kind::EntityRef)
            m_values.push_back(v.entityRef);
        else
            return Step::cancelled();
        if (m_values.size() < 3)
            return Step::cont(
                InputKind::EntitySet,
                QStringLiteral(
                    "Tool solid id, tool face index, target solid id:"));
        if (m_values.size() > 3) {
            ctx.info(QStringLiteral(
                "SPLITFACE takes exactly 3 values: tool solid id, tool face "
                "index, target solid id"));
            return Step::cancelled();
        }
        return apply(ctx);
    }

private:
    Step apply(CommandContext& ctx)
    {
        const EntityId toolId = m_values[0];
        const int faceIndex = int(m_values[1]);
        const EntityId targetId = m_values[2];

        auto* tool = dynamic_cast<SolidEntity*>(ctx.doc().entity(toolId));
        if (!tool) {
            ctx.info(QStringLiteral("SPLITFACE: tool must be a solid"));
            return Step::cancelled();
        }
        auto* target = dynamic_cast<SolidEntity*>(ctx.doc().entity(targetId));
        if (!target || targetId == toolId) {
            ctx.info(QStringLiteral(
                "SPLITFACE: target must be another solid"));
            return Step::cancelled();
        }
        const TopoDS_Shape face = faceOrReport(ctx, *tool, toolId, faceIndex);
        if (face.IsNull())
            return Step::cancelled();

        // The face may be CURVED (a cylinder wall through a box is the
        // classic use). splitByFaceExtended retries on the canonical surface
        // when the raw face fails to cut (extruded-profile lateral faces).
        const auto pieces = solidops::splitByFaceExtended(target->shape(), face);
        if (pieces.size() < 2) {
            ctx.info(QStringLiteral(
                "SPLITFACE: the face missed the solid — nothing was split"));
            return Step::cancelled();
        }
        // SPLIT's replacement block: copy the inherited fields BEFORE
        // removeEntity invalidates `target`, then swap in one transaction.
        const int64_t layerId = target->layerId();
        const ColorSpec color = target->color();
        const QString component = target->component;
        const double transparency = target->transparency;
        ctx.doc().beginTransaction(QLatin1String(name()));
        ctx.doc().removeEntity(targetId);
        EntityId firstId = 0;
        for (const TopoDS_Shape& piece : pieces) {
            const EntityId id = ctx.doc().addEntity(
                solidLike(piece, layerId, color, component, transparency));
            if (firstId == 0)
                firstId = id;
        }
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("split into %1 pieces (ids from %2)")
                     .arg(pieces.size())
                     .arg(firstId));
        return Step::done();
    }

    std::vector<EntityId> m_values; // [toolId, faceIndex, targetId]
};

// FILLETEDGES <radius> <solidId> <edgeIndex> [edgeIndex...]
// CHAMFEREDGES <distance> <solidId> <edgeIndex> [edgeIndex...]
// The per-edge, index-addressed twins of FILLET3D/CHAMFER3D. Size first (the
// EntitySet gulp would eat it); then one positional gulp — values[0] is the
// solid id, the rest are INSPECT edge indices.
class EdgeFinishByIndexCommand : public Command {
public:
    explicit EdgeFinishByIndexCommand(bool chamfer) : m_chamfer(chamfer) {}
    const char* name() const override
    {
        return m_chamfer ? "CHAMFEREDGES" : "FILLETEDGES";
    }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Distance,
                          m_chamfer ? QStringLiteral("Chamfer distance:")
                                    : QStringLiteral("Fillet radius:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (m_size <= 0) {
            if (v.kind != InputValue::Kind::Number || v.number <= kGeomTol)
                return Step::cancelled();
            m_size = v.number;
            return Step::cont(
                InputKind::EntitySet,
                QStringLiteral("Solid id then edge index(es):"));
        }
        if (v.kind == InputValue::Kind::EntitySet)
            m_values.insert(m_values.end(), v.entitySet.begin(),
                            v.entitySet.end());
        else if (v.kind == InputValue::Kind::EntityRef)
            m_values.push_back(v.entityRef);
        else {
            if (v.kind == InputValue::Kind::Finish)
                ctx.info(QStringLiteral(
                             "%1 needs a solid id and at least one edge index")
                             .arg(QLatin1String(name())));
            return Step::cancelled();
        }
        if (m_values.size() >= 2)
            return apply(ctx);
        return Step::cont(InputKind::EntitySet,
                          QStringLiteral("Edge index(es):"));
    }

private:
    Step apply(CommandContext& ctx)
    {
        const EntityId id = m_values[0];
        auto* solid = dynamic_cast<SolidEntity*>(ctx.doc().entity(id));
        if (!solid) {
            ctx.info(QStringLiteral("that id is not a solid"));
            return Step::cancelled();
        }
        std::vector<TopoDS_Shape> edges;
        for (size_t i = 1; i < m_values.size(); ++i) {
            const TopoDS_Shape edge =
                edgeOrReport(ctx, *solid, id, int(m_values[i]));
            if (edge.IsNull())
                return Step::cancelled();
            edges.push_back(edge);
        }
        const auto out =
            m_chamfer ? solidops::chamferEdges(solid->shape(), edges, m_size)
                      : solidops::filletEdges(solid->shape(), edges, m_size);
        if (!out.ok || out.shape.IsNull()) {
            ctx.info(out.message.isEmpty()
                         ? QStringLiteral("%1 failed").arg(QLatin1String(name()))
                         : out.message);
            return Step::cancelled();
        }
        ctx.doc().beginTransaction(QLatin1String(name()));
        auto* mut = static_cast<SolidEntity*>(ctx.doc().beginModify(id));
        mut->setShape(out.shape);
        ctx.doc().endModify(id);
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("%1 %2 mm on %3 edge(s) of solid %4")
                     .arg(m_chamfer ? QStringLiteral("chamfered")
                                    : QStringLiteral("filleted"))
                     .arg(m_size)
                     .arg(edges.size())
                     .arg(id));
        return Step::done();
    }

    bool m_chamfer;
    double m_size = 0.0;
    std::vector<EntityId> m_values; // [solidId, edgeIndex...]
};

// MATE <movingSolidId> <movingFaceIndex> <fixedSolidId> <fixedFaceIndex>
class MateCommand : public Command {
public:
    const char* name() const override { return "MATE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::EntitySet, prompt());
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (v.kind == InputValue::Kind::EntitySet)
            m_values.insert(m_values.end(), v.entitySet.begin(),
                            v.entitySet.end());
        else if (v.kind == InputValue::Kind::EntityRef)
            m_values.push_back(v.entityRef);
        else
            return Step::cancelled();
        if (m_values.size() < 4)
            return Step::cont(InputKind::EntitySet, prompt());
        if (m_values.size() > 4) {
            ctx.info(QStringLiteral(
                "MATE takes exactly 4 values: moving solid id, its face "
                "index, fixed solid id, its face index"));
            return Step::cancelled();
        }
        return apply(ctx);
    }

private:
    static QString prompt()
    {
        return QStringLiteral(
            "Moving solid id, its face index, fixed solid id, its face index:");
    }

    Step apply(CommandContext& ctx)
    {
        const EntityId movingId = m_values[0];
        const int movingFace = int(m_values[1]);
        const EntityId fixedId = m_values[2];
        const int fixedFace = int(m_values[3]);

        auto* moving = dynamic_cast<SolidEntity*>(ctx.doc().entity(movingId));
        if (!moving) {
            ctx.info(QStringLiteral("MATE: the moving id must be a solid"));
            return Step::cancelled();
        }
        auto* fixed = dynamic_cast<SolidEntity*>(ctx.doc().entity(fixedId));
        if (!fixed || fixedId == movingId) {
            ctx.info(QStringLiteral(
                "MATE: the fixed id must be another solid"));
            return Step::cancelled();
        }
        const TopoDS_Shape faceA =
            faceOrReport(ctx, *moving, movingId, movingFace);
        if (faceA.IsNull())
            return Step::cancelled();
        const TopoDS_Shape faceB =
            faceOrReport(ctx, *fixed, fixedId, fixedFace);
        if (faceB.IsNull())
            return Step::cancelled();

        const auto trsf = solidops::mateTransform(faceA, faceB);
        if (!trsf) {
            ctx.info(QStringLiteral(
                "MATE needs two PLANAR faces (INSPECT shows each face's "
                "surface type)"));
            return Step::cancelled();
        }
        ctx.doc().beginTransaction(QLatin1String(name()));
        auto* mut = static_cast<SolidEntity*>(ctx.doc().beginModify(movingId));
        mut->applyTrsf(*trsf);
        ctx.doc().endModify(movingId);
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral(
                     "mated face %1 of solid %2 flat onto face %3 of solid %4")
                     .arg(movingFace)
                     .arg(movingId)
                     .arg(fixedFace)
                     .arg(fixedId));
        return Step::done();
    }

    std::vector<EntityId> m_values; // [movingId, movingFace, fixedId, fixedFace]
};

// DRAFT <angleDeg> <solidId> <faceIndex> [faceIndex...]
// Default mold setup, documented in the header comment: pullDir = +Z and the
// neutral plane = the XY plane at the solid's zMin (bounding-box bottom).
class DraftCommand : public Command {
public:
    const char* name() const override { return "DRAFT"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Number,
                          QStringLiteral("Draft angle (degrees):"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (!m_haveAngle) {
            if (v.kind != InputValue::Kind::Number)
                return Step::cancelled();
            if (std::abs(v.number) <= kGeomTol || std::abs(v.number) >= 90.0) {
                ctx.info(QStringLiteral(
                    "draft angle must be non-zero and below 90 degrees"));
                return Step::cancelled();
            }
            m_angle = v.number;
            m_haveAngle = true;
            return Step::cont(
                InputKind::EntitySet,
                QStringLiteral("Solid id then face index(es):"));
        }
        if (v.kind == InputValue::Kind::EntitySet)
            m_values.insert(m_values.end(), v.entitySet.begin(),
                            v.entitySet.end());
        else if (v.kind == InputValue::Kind::EntityRef)
            m_values.push_back(v.entityRef);
        else {
            if (v.kind == InputValue::Kind::Finish)
                ctx.info(QStringLiteral(
                    "DRAFT needs a solid id and at least one face index"));
            return Step::cancelled();
        }
        if (m_values.size() >= 2)
            return apply(ctx);
        return Step::cont(InputKind::EntitySet,
                          QStringLiteral("Face index(es):"));
    }

private:
    Step apply(CommandContext& ctx)
    {
        const EntityId id = m_values[0];
        auto* solid = dynamic_cast<SolidEntity*>(ctx.doc().entity(id));
        if (!solid) {
            ctx.info(QStringLiteral("that id is not a solid"));
            return Step::cancelled();
        }
        std::vector<TopoDS_Shape> faces;
        for (size_t i = 1; i < m_values.size(); ++i) {
            const TopoDS_Shape face =
                faceOrReport(ctx, *solid, id, int(m_values[i]));
            if (face.IsNull())
                return Step::cancelled();
            faces.push_back(face);
        }
        // The documented default frame: pull along +Z, neutral plane = the
        // XY plane at the solid's bounding-box bottom (zMin), so the base
        // footprint stays put and the walls tilt from it.
        Bnd_Box bb;
        BRepBndLib::Add(solid->shape(), bb);
        if (bb.IsVoid()) {
            ctx.info(QStringLiteral("DRAFT: the solid has no extent"));
            return Step::cancelled();
        }
        double xMin, yMin, zMin, xMax, yMax, zMax;
        bb.Get(xMin, yMin, zMin, xMax, yMax, zMax);
        const gp_Dir pull(0.0, 0.0, 1.0);
        const gp_Pln neutral(gp_Pnt(0.0, 0.0, zMin), gp_Dir(0.0, 0.0, 1.0));

        const auto out =
            solidops::draftFaces(solid->shape(), faces, pull, neutral, m_angle);
        if (!out.ok || out.shape.IsNull()) {
            ctx.info(out.message.isEmpty()
                         ? QStringLiteral("draft failed")
                         : QStringLiteral("draft: %1").arg(out.message));
            return Step::cancelled();
        }
        ctx.doc().beginTransaction(QLatin1String(name()));
        auto* mut = static_cast<SolidEntity*>(ctx.doc().beginModify(id));
        mut->setShape(out.shape);
        ctx.doc().endModify(id);
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("drafted %1 face(s) of solid %2 by %3 deg "
                                "(pull +Z, neutral plane z=%4)")
                     .arg(faces.size())
                     .arg(id)
                     .arg(m_angle)
                     .arg(zMin));
        return Step::done();
    }

    bool m_haveAngle = false;
    double m_angle = 0.0;
    std::vector<EntityId> m_values; // [solidId, faceIndex...]
};

std::unique_ptr<Command> makePushPull()
{
    return std::make_unique<PushPullCommand>();
}
std::unique_ptr<Command> makeShellOpen()
{
    return std::make_unique<ShellOpenCommand>();
}
std::unique_ptr<Command> makeSplitFace()
{
    return std::make_unique<SplitFaceCommand>();
}
std::unique_ptr<Command> makeFilletEdges()
{
    return std::make_unique<EdgeFinishByIndexCommand>(false);
}
std::unique_ptr<Command> makeChamferEdges()
{
    return std::make_unique<EdgeFinishByIndexCommand>(true);
}
std::unique_ptr<Command> makeMate()
{
    return std::make_unique<MateCommand>();
}
std::unique_ptr<Command> makeDraft()
{
    return std::make_unique<DraftCommand>();
}

} // namespace

void registerSubShapeOpCommands(CommandProcessor& p)
{
    p.registerCommand(&makePushPull, {QStringLiteral("PP")});
    p.registerCommand(&makeShellOpen);
    p.registerCommand(&makeSplitFace);
    p.registerCommand(&makeFilletEdges, {QStringLiteral("FEDGES")});
    p.registerCommand(&makeChamferEdges, {QStringLiteral("CEDGES")});
    p.registerCommand(&makeMate);
    p.registerCommand(&makeDraft);
}

} // namespace viki
