#include "CommandProcessor.h"

#include <algorithm>

#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"

// 3D placement for assemblies: MOVE3D (dx dy dz) and ROTATE3D (axis angle),
// operating on selected solids. Numeric params come BEFORE the entity
// selection (the EntitySet gulp rule). Each applies a gp_Trsf to the solid's
// shape in one undoable transaction.

namespace viki {
namespace {

// Collects the trailing entity selection, then hands off to apply().
class SolidPlaceCommand : public Command {
public:
    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (!m_paramsDone)
            return onParams(ctx, v);
        // Selection phase.
        switch (v.kind) {
        case InputValue::Kind::EntitySet:
            m_ids = v.entitySet;
            return finish(ctx);
        case InputValue::Kind::EntityRef:
            m_ids.push_back(v.entityRef);
            return Step::cont(InputKind::EntitySet, selectPrompt());
        case InputValue::Kind::Finish:
            return finish(ctx);
        default:
            return Step::cont(InputKind::EntitySet, selectPrompt());
        }
    }

protected:
    virtual Step onParams(CommandContext& ctx, const InputValue& v) = 0;
    virtual gp_Trsf placement() const = 0;
    virtual QString selectPrompt() const
    {
        return QStringLiteral("Select solids (Enter to finish):");
    }

    Step beginSelection()
    {
        m_paramsDone = true;
        return Step::cont(InputKind::EntitySet, selectPrompt());
    }

    bool m_paramsDone = false;

private:
    Step finish(CommandContext& ctx)
    {
        if (m_ids.empty())
            return Step::cancelled();
        const gp_Trsf t = placement();
        ctx.doc().beginTransaction(QString::fromLatin1(name()));
        int n = 0;
        for (const EntityId id : m_ids) {
            if (!dynamic_cast<const SolidEntity*>(ctx.doc().entity(id)))
                continue; // only solids have a 3D placement
            if (auto* s = dynamic_cast<SolidEntity*>(ctx.doc().beginModify(id))) {
                s->applyTrsf(t);
                ctx.doc().endModify(id);
                ++n;
            }
        }
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("%1 solid(s) placed").arg(n));
        return Step::done();
    }

    std::vector<EntityId> m_ids;
};

// MOVE3D: either "dx dy dz solids" (numeric, unchanged) or, with keyword P
// (or a picked point) at the first prompt, "base point / destination point /
// solids". The two points are work-plane (u,v) coords — the 3D view feeds
// clicked face points as Point input — lifted to 3D on the document's work
// plane, so the translation is a true 3D gp_Vec (e.g. Y/Z on a YZ plane).
class Move3DCommand : public SolidPlaceCommand {
public:
    const char* name() const override { return "MOVE3D"; }
    Step start(CommandContext&) override
    {
        // Distance (not Number) so the prompt tolerates option keywords;
        // a bare number still parses to the same raw value in mm.
        return Step::cont(InputKind::Distance,
                          QStringLiteral("Delta X or [P]oints <0>:"));
    }

protected:
    Step onParams(CommandContext& ctx, const InputValue& v) override
    {
        if (m_axis == 0 && !m_byPoints) {
            if (v.kind == InputValue::Kind::Keyword) {
                if (v.text != QLatin1String("P"))
                    return Step::cancelled();
                m_byPoints = true;
                return Step::cont(InputKind::Point, QStringLiteral("Base point:"));
            }
            if (v.kind == InputValue::Kind::Point) {
                // A picked point at the first prompt jumps straight into
                // point mode with this as the base.
                m_byPoints = true;
                m_base = v.point;
                m_haveBase = true;
                return Step::cont(InputKind::Point,
                                  QStringLiteral("Destination point:"));
            }
        }
        if (m_byPoints) {
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            if (!m_haveBase) {
                m_base = v.point;
                m_haveBase = true;
                return Step::cont(InputKind::Point,
                                  QStringLiteral("Destination point:"));
            }
            const WorkPlane plane = documentWorkplane(ctx.doc());
            const gp_Vec d(solidops::planePoint3d(m_base, plane),
                           solidops::planePoint3d(v.point, plane));
            m_d[0] = d.X();
            m_d[1] = d.Y();
            m_d[2] = d.Z();
            return beginSelection();
        }
        const double val = v.kind == InputValue::Kind::Number ? v.number : 0.0;
        if (v.kind != InputValue::Kind::Number && v.kind != InputValue::Kind::Finish)
            return Step::cancelled();
        switch (m_axis++) {
        case 0:
            m_d[0] = val;
            return Step::cont(InputKind::Number, QStringLiteral("Delta Y <0>:"));
        case 1:
            m_d[1] = val;
            return Step::cont(InputKind::Number, QStringLiteral("Delta Z <0>:"));
        default:
            m_d[2] = val;
            return beginSelection();
        }
    }
    gp_Trsf placement() const override
    {
        gp_Trsf t;
        t.SetTranslation(gp_Vec(m_d[0], m_d[1], m_d[2]));
        return t;
    }

private:
    int m_axis = 0;
    double m_d[3] = {0, 0, 0};
    bool m_byPoints = false;
    bool m_haveBase = false;
    Vec2d m_base;
};

class Rotate3DCommand : public SolidPlaceCommand {
public:
    const char* name() const override { return "ROTATE3D"; }
    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Keyword,
                          QStringLiteral("Axis [X/Y/Z] <Z>:"));
    }

protected:
    Step onParams(CommandContext&, const InputValue& v) override
    {
        if (!m_haveAxis) {
            if (v.kind == InputValue::Kind::Keyword) {
                const QString a = v.text.toUpper();
                if (a == QLatin1String("X"))
                    m_dir = gp_Dir(1, 0, 0);
                else if (a == QLatin1String("Y"))
                    m_dir = gp_Dir(0, 1, 0);
                else
                    m_dir = gp_Dir(0, 0, 1);
            } else if (v.kind != InputValue::Kind::Finish) {
                return Step::cancelled();
            }
            m_haveAxis = true;
            return Step::cont(InputKind::Number,
                              QStringLiteral("Angle (degrees) <90>:"));
        }
        m_angle = v.kind == InputValue::Kind::Number ? v.number : 90.0;
        if (v.kind != InputValue::Kind::Number && v.kind != InputValue::Kind::Finish)
            return Step::cancelled();
        return beginSelection();
    }
    gp_Trsf placement() const override
    {
        gp_Trsf t;
        t.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), m_dir), m_angle * M_PI / 180.0);
        return t;
    }

private:
    bool m_haveAxis = false;
    gp_Dir m_dir{0, 0, 1};
    double m_angle = 90.0;
};

// TRANSPARENCY <percent> <solids> — 0 = opaque, 100 = (nearly) invisible.
class TransparencyCommand : public Command {
public:
    const char* name() const override { return "TRANSPARENCY"; }
    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Number,
                          QStringLiteral("Transparency %% (0-95) <50>:"));
    }
    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (!m_havePct) {
            if (v.kind == InputValue::Kind::Number)
                m_pct = std::clamp(v.number, 0.0, 95.0);
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_havePct = true;
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select solids (Enter to finish):"));
        }
        switch (v.kind) {
        case InputValue::Kind::EntitySet:
            m_ids = v.entitySet;
            return apply(ctx);
        case InputValue::Kind::EntityRef:
            m_ids.push_back(v.entityRef);
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select solids (Enter to finish):"));
        case InputValue::Kind::Finish:
            return apply(ctx);
        default:
            return Step::cont(InputKind::EntitySet,
                              QStringLiteral("Select solids (Enter to finish):"));
        }
    }

private:
    Step apply(CommandContext& ctx)
    {
        if (m_ids.empty())
            return Step::cancelled();
        ctx.doc().beginTransaction(QStringLiteral("TRANSPARENCY"));
        int n = 0;
        for (const EntityId id : m_ids) {
            if (!dynamic_cast<const SolidEntity*>(ctx.doc().entity(id)))
                continue;
            if (auto* s = dynamic_cast<SolidEntity*>(ctx.doc().beginModify(id))) {
                s->transparency = m_pct / 100.0;
                ctx.doc().endModify(id);
                ++n;
            }
        }
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("%1 solid(s) set to %2%% transparent")
                     .arg(n).arg(int(m_pct)));
        return Step::done();
    }

    bool m_havePct = false;
    double m_pct = 50.0;
    std::vector<EntityId> m_ids;
};

std::unique_ptr<Command> makeMove3D() { return std::make_unique<Move3DCommand>(); }
std::unique_ptr<Command> makeRotate3D() { return std::make_unique<Rotate3DCommand>(); }
std::unique_ptr<Command> makeTransparency()
{
    return std::make_unique<TransparencyCommand>();
}

} // namespace

void registerAssemblyCommands(CommandProcessor& p)
{
    p.registerCommand(&makeMove3D, {QStringLiteral("M3")});
    p.registerCommand(&makeRotate3D, {QStringLiteral("RO3")});
    p.registerCommand(&makeTransparency, {QStringLiteral("TRANS")});
}

} // namespace viki
