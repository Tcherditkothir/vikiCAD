#include "CommandProcessor.h"

#include "doc/Block.h"
#include "render/HitTest.h"

// M5: BLOCK (define from entities), INSERT (place a reference),
// ATTDEF (attribute definition inside a future block).

namespace viki {
namespace {

// BLOCK name base_point entity-ids  — converts entities into a definition
// and replaces them with one insert (common "make block" expectation).
class BlockCommand : public Command {
public:
    const char* name() const override { return "BLOCK"; }

    Step start(CommandContext& ctx) override
    {
        if (!ctx.selection().isEmpty()) {
            m_ids = ctx.selection().ids();
            ctx.selection().clear();
        }
        return Step::cont(InputKind::Keyword, QStringLiteral("Block name:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_stage) {
        case 0:
            if (v.kind != InputValue::Kind::Keyword || v.text.isEmpty())
                return Step::cancelled();
            if (ctx.doc().blockByName(v.text)) {
                ctx.info(QStringLiteral("block '%1' already exists").arg(v.text));
                return Step::cancelled();
            }
            m_name = v.text;
            m_stage = 1;
            return Step::cont(InputKind::Point, QStringLiteral("Base point:"));
        case 1:
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            m_base = v.point;
            if (!m_ids.empty())
                return build(ctx);
            m_stage = 2;
            return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
        case 2:
            if (v.kind == InputValue::Kind::EntitySet) {
                m_ids = v.entitySet;
                return build(ctx);
            }
            if (v.kind == InputValue::Kind::EntityRef) {
                m_ids.push_back(v.entityRef);
                return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
            }
            if (v.kind == InputValue::Kind::Finish && !m_ids.empty())
                return build(ctx);
            return Step::cancelled();
        default:
            return Step::cancelled();
        }
    }

private:
    Step build(CommandContext& ctx)
    {
        // Definition is created directly; the model-space swap is journaled.
        BlockDef* def = ctx.doc().createBlock(m_name, m_base);
        ctx.doc().beginTransaction(QStringLiteral("BLOCK"));
        int moved = 0;
        for (const EntityId id : m_ids) {
            const Entity* e = ctx.doc().entity(id);
            if (!e)
                continue;
            def->entities.push_back(e->clone());
            ctx.doc().removeEntity(id);
            ++moved;
        }
        auto insert = std::make_unique<InsertEntity>();
        insert->blockName = m_name;
        insert->position = m_base;
        ctx.doc().addEntity(std::move(insert));
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("block '%1' created from %2 entities").arg(m_name).arg(moved));
        return Step::done();
    }

    int m_stage = 0;
    QString m_name;
    Vec2d m_base;
    std::vector<EntityId> m_ids;
};

// INSERT name position [scale] [rotation_deg], then attribute values.
class InsertCommand : public Command {
public:
    const char* name() const override { return "INSERT"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Keyword, QStringLiteral("Block name:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_stage) {
        case 0: {
            if (v.kind != InputValue::Kind::Keyword)
                return Step::cancelled();
            const BlockDef* def = ctx.doc().blockByName(v.text);
            if (!def) {
                ctx.info(QStringLiteral("no block named '%1'").arg(v.text));
                return Step::cancelled();
            }
            m_name = def->name;
            for (const auto& e : def->entities)
                if (const auto* ad = dynamic_cast<const AttDefEntity*>(e.get()))
                    m_pendingTags.push_back({ad->tag(), ad->defaultValue()});
            m_stage = 1;
            return Step::cont(InputKind::Point, QStringLiteral("Insertion point:"));
        }
        case 1:
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            m_pos = v.point;
            ctx.setLastPoint(v.point);
            m_stage = 2;
            return Step::cont(InputKind::Number, QStringLiteral("Scale <1>:"));
        case 2:
            if (v.kind == InputValue::Kind::Number)
                m_scale = v.number > kGeomTol ? v.number : 1.0;
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_stage = 3;
            return Step::cont(InputKind::Number, QStringLiteral("Rotation (degrees) <0>:"));
        case 3:
            if (v.kind == InputValue::Kind::Number)
                m_rotation = v.number * M_PI / 180.0;
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_stage = 4;
            return nextAttrOrBuild(ctx);
        case 4:
            if (v.kind == InputValue::Kind::Text)
                m_attrs[m_pendingTags[size_t(m_tagIndex)].first] = v.text;
            else if (v.kind == InputValue::Kind::Finish)
                m_attrs[m_pendingTags[size_t(m_tagIndex)].first] =
                    m_pendingTags[size_t(m_tagIndex)].second;
            else
                return Step::cancelled();
            ++m_tagIndex;
            return nextAttrOrBuild(ctx);
        default:
            return Step::cancelled();
        }
    }

    void previewAt(CommandContext& ctx, const Vec2d& cursor, PrimitiveList& out) override
    {
        if (m_stage != 1 || m_name.isEmpty())
            return;
        InsertEntity ghost;
        ghost.blockName = m_name;
        ghost.position = cursor;
        RenderContext rc;
        rc.doc = &ctx.doc();
        rc.chordTolerance = 0.5;
        ghost.buildPrimitives(rc, out);
    }

private:
    Step nextAttrOrBuild(CommandContext& ctx)
    {
        if (m_tagIndex < int(m_pendingTags.size()))
            return Step::cont(InputKind::Text,
                              QStringLiteral("Value for %1 <%2>:")
                                  .arg(m_pendingTags[size_t(m_tagIndex)].first,
                                       m_pendingTags[size_t(m_tagIndex)].second));
        auto insert = std::make_unique<InsertEntity>();
        insert->blockName = m_name;
        insert->position = m_pos;
        insert->scale = m_scale;
        insert->rotation = m_rotation;
        insert->attributes = m_attrs;
        ctx.doc().beginTransaction(QStringLiteral("INSERT"));
        ctx.doc().addEntity(std::move(insert));
        ctx.doc().commitTransaction();
        return Step::done();
    }

    int m_stage = 0;
    QString m_name;
    Vec2d m_pos;
    double m_scale = 1.0;
    double m_rotation = 0.0;
    std::vector<std::pair<QString, QString>> m_pendingTags;
    int m_tagIndex = 0;
    QJsonObject m_attrs;
};

// ATTDEF tag default position height — a plain entity; BLOCK moves it into
// the definition like anything else.
class AttDefCommand : public Command {
public:
    const char* name() const override { return "ATTDEF"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Keyword, QStringLiteral("Attribute tag:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_stage) {
        case 0:
            if (v.kind != InputValue::Kind::Keyword)
                return Step::cancelled();
            m_tag = v.text;
            m_stage = 1;
            return Step::cont(InputKind::Keyword,
                              QStringLiteral("Default value (Enter for none):"));
        case 1:
            if (v.kind == InputValue::Kind::Keyword)
                m_default = v.text;
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_stage = 2;
            return Step::cont(InputKind::Point, QStringLiteral("Position:"));
        case 2:
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            m_pos = v.point;
            m_stage = 3;
            return Step::cont(InputKind::Distance, QStringLiteral("Text height <3.5>:"));
        case 3: {
            double h = 3.5;
            if (v.kind == InputValue::Kind::Number)
                h = std::max(0.1, v.number);
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            ctx.doc().beginTransaction(QStringLiteral("ATTDEF"));
            ctx.doc().addEntity(
                std::make_unique<AttDefEntity>(m_pos, h, m_tag, m_default));
            ctx.doc().commitTransaction();
            return Step::done();
        }
        default:
            return Step::cancelled();
        }
    }

private:
    int m_stage = 0;
    QString m_tag, m_default;
    Vec2d m_pos;
};

template <typename T>
std::unique_ptr<Command> make()
{
    return std::make_unique<T>();
}

} // namespace

void registerBlockCommands(CommandProcessor& p)
{
    p.registerCommand(&make<BlockCommand>, {QStringLiteral("B")});
    p.registerCommand(&make<InsertCommand>, {QStringLiteral("I")});
    p.registerCommand(&make<AttDefCommand>, {QStringLiteral("ATT")});
}

} // namespace viki
