#include "CommandProcessor.h"

#include "geom/GeomUtil.h"

// MOVE / COPY / ROTATE / MIRROR / SCALE — all share the same shape:
// acquire an entity set (pickfirst or prompt), gather transform parameters,
// then apply one journaled transaction over the set.

namespace viki {
namespace {

// Base class handling the selection stage uniformly.
class ModifyCommand : public Command {
public:
    Step start(CommandContext& ctx) override
    {
        if (!ctx.selection().isEmpty()) {
            m_ids = ctx.selection().ids();
            ctx.selection().clear();
            return firstParamStep(ctx);
        }
        return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();

        if (!m_selectionDone) {
            switch (v.kind) {
            case InputValue::Kind::EntitySet:
                m_ids = v.entitySet;
                return firstParamStepChecked(ctx);
            case InputValue::Kind::EntityRef:
                m_ids.push_back(v.entityRef);
                return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
            case InputValue::Kind::Finish:
                return firstParamStepChecked(ctx);
            default:
                return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
            }
        }
        return onParamInput(ctx, v);
    }

protected:
    virtual Step firstParamStep(CommandContext& ctx) = 0;
    virtual Step onParamInput(CommandContext& ctx, const InputValue& v) = 0;

    // Applies xf to the whole set in one transaction. copy=true duplicates.
    void applyToSet(CommandContext& ctx, const Xform2d& xf, bool copy,
                    const QString& txName)
    {
        ctx.doc().beginTransaction(txName);
        int n = 0;
        for (const EntityId id : m_ids) {
            if (copy) {
                const Entity* src = ctx.doc().entity(id);
                if (!src)
                    continue;
                auto dup = src->clone();
                dup->transform(xf);
                ctx.doc().addEntity(std::move(dup));
                ++n;
            } else {
                Entity* e = ctx.doc().beginModify(id);
                if (!e)
                    continue;
                e->transform(xf);
                ctx.doc().endModify(id);
                ++n;
            }
        }
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("%1 object(s) %2").arg(n).arg(
            copy ? QStringLiteral("copied") : QStringLiteral("transformed")));
    }

    // Ghost preview of the set under transform.
    void previewSet(CommandContext& ctx, const Xform2d& xf, PrimitiveList& out)
    {
        RenderContext rc; // coarse preview flattening is fine
        rc.chordTolerance = 0.5;
        for (const EntityId id : m_ids) {
            const Entity* e = ctx.doc().entity(id);
            if (!e)
                continue;
            auto ghost = e->clone();
            ghost->transform(xf);
            ghost->buildPrimitives(rc, out);
        }
    }

    std::vector<EntityId> m_ids;
    bool m_selectionDone = false;

private:
    Step firstParamStepChecked(CommandContext& ctx)
    {
        if (m_ids.empty())
            return Step::cancelled();
        m_selectionDone = true;
        return firstParamStep(ctx);
    }
};

// MOVE (and COPY, which only differs by duplication).
class MoveCommand : public ModifyCommand {
public:
    explicit MoveCommand(bool copy) : m_copy(copy) {}
    const char* name() const override { return m_copy ? "COPY" : "MOVE"; }

protected:
    Step firstParamStep(CommandContext&) override
    {
        m_selectionDone = true;
        return Step::cont(InputKind::Point, QStringLiteral("Specify base point:"));
    }

    Step onParamInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind != InputValue::Kind::Point)
            return Step::cancelled();
        if (!m_hasBase) {
            m_base = v.point;
            m_hasBase = true;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point, QStringLiteral("Specify second point:"));
        }
        applyToSet(ctx, Xform2d::translation(v.point - m_base), m_copy,
                   QLatin1String(name()));
        ctx.setLastPoint(v.point);
        return Step::done();
    }

    void previewAt(CommandContext& ctx, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (m_hasBase)
            previewSet(ctx, Xform2d::translation(cursor - m_base), out);
    }

private:
    bool m_copy;
    Vec2d m_base;
    bool m_hasBase = false;
};

class RotateCommand : public ModifyCommand {
public:
    const char* name() const override { return "ROTATE"; }

protected:
    Step firstParamStep(CommandContext&) override
    {
        m_selectionDone = true;
        return Step::cont(InputKind::Point, QStringLiteral("Specify base point:"));
    }

    Step onParamInput(CommandContext& ctx, const InputValue& v) override
    {
        if (!m_hasBase) {
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            m_base = v.point;
            m_hasBase = true;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Number,
                              QStringLiteral("Specify rotation angle (degrees):"));
        }
        double radians = 0;
        if (v.kind == InputValue::Kind::Number)
            radians = v.number * M_PI / 180.0;
        else if (v.kind == InputValue::Kind::Point)
            radians = (v.point - m_base).angle();
        else
            return Step::cancelled();
        applyToSet(ctx, Xform2d::rotation(radians, m_base), false, QStringLiteral("ROTATE"));
        return Step::done();
    }

    void previewAt(CommandContext& ctx, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (m_hasBase && !nearEqual(cursor, m_base))
            previewSet(ctx, Xform2d::rotation((cursor - m_base).angle(), m_base), out);
    }

private:
    Vec2d m_base;
    bool m_hasBase = false;
};

class MirrorCommand : public ModifyCommand {
public:
    const char* name() const override { return "MIRROR"; }

protected:
    Step firstParamStep(CommandContext&) override
    {
        m_selectionDone = true;
        return Step::cont(InputKind::Point,
                          QStringLiteral("Specify first point of mirror line:"));
    }

    Step onParamInput(CommandContext& ctx, const InputValue& v) override
    {
        switch (m_stage) {
        case 0:
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            m_p1 = v.point;
            m_stage = 1;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point,
                              QStringLiteral("Specify second point of mirror line:"));
        case 1:
            if (v.kind != InputValue::Kind::Point || nearEqual(v.point, m_p1))
                return Step::cancelled();
            m_p2 = v.point;
            m_stage = 2;
            return Step::cont(InputKind::Keyword,
                              QStringLiteral("Erase source objects? [Yes/No] <No>:"));
        case 2: {
            bool erase = false;
            if (v.kind == InputValue::Kind::Keyword)
                erase = v.text.startsWith(QLatin1Char('Y'));
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            applyMirror(ctx, erase);
            return Step::done();
        }
        default:
            return Step::cancelled();
        }
    }

    void previewAt(CommandContext& ctx, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (m_stage == 1 && !nearEqual(cursor, m_p1))
            previewSet(ctx, mirrorXform(m_p1, cursor), out);
    }

private:
    static Xform2d mirrorXform(const Vec2d& a, const Vec2d& b)
    {
        const double theta = (b - a).angle();
        const double c = std::cos(2 * theta);
        const double s = std::sin(2 * theta);
        // Reflection about the line through a with direction angle theta:
        // T(a) * Reflect(2*theta) * T(-a)
        Xform2d m{c, s, s, -c, 0, 0};
        m.tx = a.x - (c * a.x + s * a.y);
        m.ty = a.y - (s * a.x - c * a.y);
        return m;
    }

    void applyMirror(CommandContext& ctx, bool erase)
    {
        const Xform2d m = mirrorXform(m_p1, m_p2);
        // Mirror always creates copies; "erase source" removes the originals.
        ctx.doc().beginTransaction(QStringLiteral("MIRROR"));
        int n = 0;
        for (const EntityId id : m_ids) {
            const Entity* src = ctx.doc().entity(id);
            if (!src)
                continue;
            auto dup = src->clone();
            dup->transform(m);
            ctx.doc().addEntity(std::move(dup));
            if (erase)
                ctx.doc().removeEntity(id);
            ++n;
        }
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("%1 object(s) mirrored").arg(n));
    }

    int m_stage = 0;
    Vec2d m_p1, m_p2;
};

class ScaleCommand : public ModifyCommand {
public:
    const char* name() const override { return "SCALE"; }

protected:
    Step firstParamStep(CommandContext&) override
    {
        m_selectionDone = true;
        return Step::cont(InputKind::Point, QStringLiteral("Specify base point:"));
    }

    Step onParamInput(CommandContext& ctx, const InputValue& v) override
    {
        if (!m_hasBase) {
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            m_base = v.point;
            m_hasBase = true;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Number, QStringLiteral("Scale factor (type a value; a click uses its distance to base):"));
        }
        double factor = 0;
        if (v.kind == InputValue::Kind::Number)
            factor = v.number;
        else if (v.kind == InputValue::Kind::Point)
            factor = v.point.distanceTo(m_base);
        if (factor <= kGeomTol) {
            ctx.info(QStringLiteral("scale factor must be positive"));
            return Step::cont(InputKind::Number, QStringLiteral("Scale factor (type a value; a click uses its distance to base):"));
        }
        applyToSet(ctx, Xform2d::scaling(factor, m_base), false, QStringLiteral("SCALE"));
        return Step::done();
    }

    void previewAt(CommandContext& ctx, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (!m_hasBase)
            return;
        const double factor = cursor.distanceTo(m_base);
        if (factor > kGeomTol && factor < 1e4)
            previewSet(ctx, Xform2d::scaling(factor, m_base), out);
    }

private:
    Vec2d m_base;
    bool m_hasBase = false;
};

std::unique_ptr<Command> makeMove() { return std::make_unique<MoveCommand>(false); }
std::unique_ptr<Command> makeCopy() { return std::make_unique<MoveCommand>(true); }

template <typename T>
std::unique_ptr<Command> make()
{
    return std::make_unique<T>();
}

} // namespace

void registerModifyCommands(CommandProcessor& p)
{
    p.registerCommand(&makeMove, {QStringLiteral("M")});
    p.registerCommand(&makeCopy, {QStringLiteral("CO"), QStringLiteral("CP")});
    p.registerCommand(&make<RotateCommand>, {QStringLiteral("RO")});
    p.registerCommand(&make<MirrorCommand>, {QStringLiteral("MI")});
    p.registerCommand(&make<ScaleCommand>, {QStringLiteral("SC")});
}

} // namespace viki
