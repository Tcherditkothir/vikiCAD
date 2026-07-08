#include "CommandProcessor.h"

#include "edit/EditOps.h"
#include "render/HitTest.h"

// M3 editing commands. All entity picking is point-driven (hittest at the
// given point with ctx.pickTolerance) so the exact same command text works
// from the GUI, scripts and the headless CLI.

namespace viki {
namespace {

EntityId pickAt(CommandContext& ctx, const Vec2d& p)
{
    return hittest::pick(ctx.doc(), p, ctx.pickTolerance());
}

// Runs `op` inside its own transaction, reporting the message.
template <typename Fn>
void runOp(CommandContext& ctx, const QString& txName, Fn&& op)
{
    ctx.doc().beginTransaction(txName);
    const editops::OpResult r = op();
    if (r.ok) {
        ctx.doc().commitTransaction();
        if (!r.message.isEmpty())
            ctx.info(r.message);
    } else {
        ctx.doc().rollbackTransaction();
        ctx.info(r.message);
    }
}

// ---- TRIM / EXTEND (same shape: edge set, then pick loop) --------------------

class TrimExtendCommand : public Command {
public:
    explicit TrimExtendCommand(bool extend) : m_extend(extend) {}
    const char* name() const override { return m_extend ? "EXTEND" : "TRIM"; }

    Step start(CommandContext& ctx) override
    {
        if (!ctx.selection().isEmpty()) {
            m_edges = ctx.selection().ids();
            ctx.selection().clear();
            m_edgesDone = true;
            return pickPrompt();
        }
        return Step::cont(InputKind::EntitySet,
                          m_extend
                              ? QStringLiteral("Select boundary edges (Enter for all):")
                              : QStringLiteral("Select cutting edges (Enter for all):"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();

        if (!m_edgesDone) {
            switch (v.kind) {
            case InputValue::Kind::EntitySet:
                m_edges = v.entitySet;
                m_edgesDone = true;
                return pickPrompt();
            case InputValue::Kind::EntityRef:
                m_edges.push_back(v.entityRef);
                return Step::cont(InputKind::EntitySet, QStringLiteral("Select edges:"));
            case InputValue::Kind::Finish: // empty = all entities are edges
                m_edgesDone = true;
                return pickPrompt();
            default:
                return Step::cont(InputKind::EntitySet, QStringLiteral("Select edges:"));
            }
        }

        if (v.kind == InputValue::Kind::Finish)
            return m_ops > 0 ? Step::done() : Step::cancelled();
        if (v.kind != InputValue::Kind::Point)
            return pickPrompt();

        const EntityId target = pickAt(ctx, v.point);
        if (target == kInvalidEntityId) {
            ctx.info(QStringLiteral("nothing there"));
            return pickPrompt();
        }
        runOp(ctx, QLatin1String(name()), [&] {
            return m_extend
                       ? editops::extendEntity(ctx.doc(), target, m_edges, v.point)
                       : editops::trimEntity(ctx.doc(), target, m_edges, v.point);
        });
        ++m_ops;
        return pickPrompt();
    }

private:
    Step pickPrompt() const
    {
        return Step::cont(InputKind::Point,
                          m_extend ? QStringLiteral("Pick entity to extend (Enter to finish):")
                                   : QStringLiteral("Pick portion to trim (Enter to finish):"));
    }
    bool m_extend;
    std::vector<EntityId> m_edges;
    bool m_edgesDone = false;
    int m_ops = 0;
};

// ---- OFFSET -------------------------------------------------------------------

class OffsetCommand : public Command {
public:
    const char* name() const override { return "OFFSET"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Distance, QStringLiteral("Specify offset distance:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();

        switch (m_stage) {
        case 0:
            if (v.kind != InputValue::Kind::Number)
                return Step::cancelled();
            m_distance = v.number;
            m_stage = 1;
            return Step::cont(InputKind::Point,
                              QStringLiteral("Pick entity to offset (Enter to finish):"));
        case 1: {
            if (v.kind == InputValue::Kind::Finish)
                return m_ops > 0 ? Step::done() : Step::cancelled();
            if (v.kind != InputValue::Kind::Point)
                return Step::cont(InputKind::Point, QStringLiteral("Pick entity:"));
            m_source = pickAt(ctx, v.point);
            if (m_source == kInvalidEntityId) {
                ctx.info(QStringLiteral("nothing there"));
                return Step::cont(InputKind::Point, QStringLiteral("Pick entity:"));
            }
            m_stage = 2;
            return Step::cont(InputKind::Point, QStringLiteral("Pick side to offset:"));
        }
        case 2:
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            runOp(ctx, QStringLiteral("OFFSET"), [&] {
                return editops::offsetEntity(ctx.doc(), m_source, m_distance, v.point);
            });
            ++m_ops;
            m_stage = 1;
            return Step::cont(InputKind::Point,
                              QStringLiteral("Pick entity to offset (Enter to finish):"));
        default:
            return Step::cancelled();
        }
    }

    void previewAt(CommandContext& ctx, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (m_stage != 2 || m_source == kInvalidEntityId)
            return;
        auto ghost = editops::offsetGeometry(ctx.doc(), m_source, m_distance, cursor);
        if (!ghost)
            return;
        RenderContext rc;
        rc.chordTolerance = 0.5;
        ghost->buildPrimitives(rc, out);
    }

private:
    int m_stage = 0;
    double m_distance = 1.0;
    EntityId m_source = kInvalidEntityId;
    int m_ops = 0;
};

// ---- FILLET / CHAMFER ------------------------------------------------------------

class FilletCommand : public Command {
public:
    const char* name() const override { return "FILLET"; }

    Step start(CommandContext&) override { return firstPrompt(); }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel || v.kind == InputValue::Kind::Finish)
            return Step::cancelled();

        switch (m_stage) {
        case 0:
            if (v.kind == InputValue::Kind::Keyword && v.text == QLatin1String("R")) {
                m_stage = 1;
                return Step::cont(InputKind::Distance, QStringLiteral("Specify radius:"));
            }
            if (v.kind == InputValue::Kind::Point) {
                m_id1 = pickAt(ctx, v.point);
                if (m_id1 == kInvalidEntityId) {
                    ctx.info(QStringLiteral("nothing there"));
                    return firstPrompt();
                }
                m_pick1 = v.point;
                m_stage = 2;
                return Step::cont(InputKind::Point, QStringLiteral("Pick second line:"));
            }
            return firstPrompt();
        case 1:
            if (v.kind != InputValue::Kind::Number)
                return Step::cancelled();
            m_radius = std::max(0.0, v.number);
            m_stage = 0;
            return firstPrompt();
        case 2: {
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            const EntityId id2 = pickAt(ctx, v.point);
            if (id2 == kInvalidEntityId) {
                ctx.info(QStringLiteral("nothing there"));
                return Step::cont(InputKind::Point, QStringLiteral("Pick second line:"));
            }
            runOp(ctx, QStringLiteral("FILLET"), [&] {
                return editops::filletLines(ctx.doc(), m_id1, m_pick1, id2, v.point,
                                            m_radius);
            });
            return Step::done();
        }
        default:
            return Step::cancelled();
        }
    }

private:
    Step firstPrompt() const
    {
        return Step::cont(InputKind::Point,
                          QStringLiteral("Pick first line or [R for radius=%1]:")
                              .arg(m_radius));
    }
    int m_stage = 0;
    double m_radius = 0.0;
    EntityId m_id1 = kInvalidEntityId;
    Vec2d m_pick1;
};

class ChamferCommand : public Command {
public:
    const char* name() const override { return "CHAMFER"; }

    Step start(CommandContext&) override { return firstPrompt(); }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel || v.kind == InputValue::Kind::Finish)
            return Step::cancelled();

        switch (m_stage) {
        case 0:
            if (v.kind == InputValue::Kind::Keyword && v.text == QLatin1String("D")) {
                m_stage = 1;
                return Step::cont(InputKind::Distance,
                                  QStringLiteral("Specify first distance:"));
            }
            if (v.kind == InputValue::Kind::Point) {
                m_id1 = pickAt(ctx, v.point);
                if (m_id1 == kInvalidEntityId) {
                    ctx.info(QStringLiteral("nothing there"));
                    return firstPrompt();
                }
                m_pick1 = v.point;
                m_stage = 3;
                return Step::cont(InputKind::Point, QStringLiteral("Pick second line:"));
            }
            return firstPrompt();
        case 1:
            if (v.kind != InputValue::Kind::Number)
                return Step::cancelled();
            m_d1 = std::max(0.0, v.number);
            m_stage = 2;
            return Step::cont(InputKind::Distance,
                              QStringLiteral("Specify second distance <%1>:").arg(m_d1));
        case 2:
            if (v.kind == InputValue::Kind::Number)
                m_d2 = std::max(0.0, v.number);
            else
                m_d2 = m_d1;
            m_stage = 0;
            return firstPrompt();
        case 3: {
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            const EntityId id2 = pickAt(ctx, v.point);
            if (id2 == kInvalidEntityId) {
                ctx.info(QStringLiteral("nothing there"));
                return Step::cont(InputKind::Point, QStringLiteral("Pick second line:"));
            }
            runOp(ctx, QStringLiteral("CHAMFER"), [&] {
                return editops::chamferLines(ctx.doc(), m_id1, m_pick1, id2, v.point, m_d1,
                                             m_d2);
            });
            return Step::done();
        }
        default:
            return Step::cancelled();
        }
    }

private:
    Step firstPrompt() const
    {
        return Step::cont(InputKind::Point,
                          QStringLiteral("Pick first line or [D for distances=%1,%2]:")
                              .arg(m_d1)
                              .arg(m_d2));
    }
    int m_stage = 0;
    double m_d1 = 0.0, m_d2 = 0.0;
    EntityId m_id1 = kInvalidEntityId;
    Vec2d m_pick1;
};

// ---- BREAK --------------------------------------------------------------------

class BreakCommand : public Command {
public:
    const char* name() const override { return "BREAK"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point,
                          QStringLiteral("Pick entity at first break point:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();

        if (m_stage == 0) {
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            m_target = pickAt(ctx, v.point);
            if (m_target == kInvalidEntityId) {
                ctx.info(QStringLiteral("nothing there"));
                return Step::cont(InputKind::Point,
                                  QStringLiteral("Pick entity at first break point:"));
            }
            m_p1 = v.point;
            m_stage = 1;
            return Step::cont(InputKind::Point,
                              QStringLiteral("Second break point (Enter = split only):"));
        }
        Vec2d p2 = m_p1;
        if (v.kind == InputValue::Kind::Point)
            p2 = v.point;
        else if (v.kind != InputValue::Kind::Finish)
            return Step::cancelled();
        runOp(ctx, QStringLiteral("BREAK"), [&] {
            return editops::breakEntity(ctx.doc(), m_target, m_p1, p2);
        });
        return Step::done();
    }

private:
    int m_stage = 0;
    EntityId m_target = kInvalidEntityId;
    Vec2d m_p1;
};

// ---- JOIN / EXPLODE (set-based) -------------------------------------------------

class SetOpCommand : public Command {
public:
    explicit SetOpCommand(bool join) : m_join(join) {}
    const char* name() const override { return m_join ? "JOIN" : "EXPLODE"; }

    Step start(CommandContext& ctx) override
    {
        if (!ctx.selection().isEmpty()) {
            const auto ids = ctx.selection().ids();
            ctx.selection().clear();
            apply(ctx, ids);
            return Step::done();
        }
        return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        switch (v.kind) {
        case InputValue::Kind::EntitySet:
            apply(ctx, v.entitySet);
            return Step::done();
        case InputValue::Kind::EntityRef:
            m_picked.push_back(v.entityRef);
            return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
        case InputValue::Kind::Finish:
            if (m_picked.empty())
                return Step::cancelled();
            apply(ctx, m_picked);
            return Step::done();
        default:
            return Step::cancelled();
        }
    }

private:
    void apply(CommandContext& ctx, const std::vector<EntityId>& ids)
    {
        runOp(ctx, QLatin1String(name()), [&] {
            return m_join ? editops::joinEntities(ctx.doc(), ids)
                          : editops::explodeEntities(ctx.doc(), ids);
        });
    }
    bool m_join;
    std::vector<EntityId> m_picked;
};

// ---- STRETCH -------------------------------------------------------------------

class StretchCommand : public Command {
public:
    const char* name() const override { return "STRETCH"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point,
                          QStringLiteral("First corner of crossing window:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind != InputValue::Kind::Point)
            return Step::cancelled();
        switch (m_stage) {
        case 0:
            m_c1 = v.point;
            m_stage = 1;
            return Step::cont(InputKind::Point, QStringLiteral("Opposite corner:"));
        case 1: {
            m_window = BBox2d(m_c1, v.point);
            m_ids = hittest::crossing(ctx.doc(), m_window);
            if (m_ids.empty()) {
                ctx.info(QStringLiteral("nothing in the window"));
                return Step::cancelled();
            }
            m_stage = 2;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point, QStringLiteral("Specify base point:"));
        }
        case 2:
            m_base = v.point;
            m_stage = 3;
            ctx.setLastPoint(v.point);
            return Step::cont(InputKind::Point, QStringLiteral("Specify second point:"));
        case 3:
            runOp(ctx, QStringLiteral("STRETCH"), [&] {
                return editops::stretchEntities(ctx.doc(), m_ids, m_window,
                                                v.point - m_base);
            });
            return Step::done();
        default:
            return Step::cancelled();
        }
    }

private:
    int m_stage = 0;
    Vec2d m_c1, m_base;
    BBox2d m_window;
    std::vector<EntityId> m_ids;
};

// ---- MATCHPROP -----------------------------------------------------------------

class MatchPropCommand : public Command {
public:
    const char* name() const override { return "MATCHPROP"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Pick source object:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (v.kind == InputValue::Kind::Finish)
            return m_count > 0 ? Step::done() : Step::cancelled();
        if (v.kind != InputValue::Kind::Point)
            return Step::cancelled();

        const EntityId id = pickAt(ctx, v.point);
        if (id == kInvalidEntityId) {
            ctx.info(QStringLiteral("nothing there"));
            return Step::cont(InputKind::Point,
                              m_source == kInvalidEntityId
                                  ? QStringLiteral("Pick source object:")
                                  : QStringLiteral("Pick destination (Enter to finish):"));
        }
        if (m_source == kInvalidEntityId) {
            m_source = id;
            return Step::cont(InputKind::Point,
                              QStringLiteral("Pick destination (Enter to finish):"));
        }
        const Entity* src = ctx.doc().entity(m_source);
        if (src && id != m_source) {
            ctx.doc().beginTransaction(QStringLiteral("MATCHPROP"));
            if (Entity* dst = ctx.doc().beginModify(id)) {
                dst->setLayerId(src->layerId());
                dst->setColor(src->color());
                ctx.doc().endModify(id);
            }
            ctx.doc().commitTransaction();
            ++m_count;
        }
        return Step::cont(InputKind::Point,
                          QStringLiteral("Pick destination (Enter to finish):"));
    }

private:
    EntityId m_source = kInvalidEntityId;
    int m_count = 0;
};

std::unique_ptr<Command> makeTrim() { return std::make_unique<TrimExtendCommand>(false); }
std::unique_ptr<Command> makeExtend() { return std::make_unique<TrimExtendCommand>(true); }
std::unique_ptr<Command> makeJoin() { return std::make_unique<SetOpCommand>(true); }
std::unique_ptr<Command> makeExplode() { return std::make_unique<SetOpCommand>(false); }

template <typename T>
std::unique_ptr<Command> make()
{
    return std::make_unique<T>();
}

} // namespace

void registerEditCommands(CommandProcessor& p)
{
    p.registerCommand(&makeTrim, {QStringLiteral("TR")});
    p.registerCommand(&makeExtend, {QStringLiteral("EX")});
    p.registerCommand(&make<OffsetCommand>, {QStringLiteral("O")});
    p.registerCommand(&make<FilletCommand>, {QStringLiteral("F")});
    p.registerCommand(&make<ChamferCommand>, {QStringLiteral("CHA")});
    p.registerCommand(&make<BreakCommand>, {QStringLiteral("BR")});
    p.registerCommand(&makeJoin, {QStringLiteral("J")});
    p.registerCommand(&makeExplode, {QStringLiteral("X")});
    p.registerCommand(&make<StretchCommand>, {QStringLiteral("S")});
    p.registerCommand(&make<MatchPropCommand>, {QStringLiteral("MA")});
}

} // namespace viki
