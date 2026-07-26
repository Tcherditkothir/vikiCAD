#include "CommandProcessor.h"

#include <BRep_Builder.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include "io/ClipboardIo.h"
#include "solid/SolidEntity.h"

// COPYCLIP / CUTCLIP / PASTECLIP — the system-clipboard trio behind
// Ctrl+C/X/V. The payload is self-contained (io/ClipboardIo.h), so pasting
// works in the same document, in another document, or in another VikiCAD
// process. Front ends without a clipboard hook share a process-local buffer.

namespace viki {
namespace {

QByteArray readClipboardData(CommandContext& ctx)
{
    if (ClipboardHook* cb = ctx.clipboard())
        return cb->data();
    return processClipboardBuffer();
}

void writeClipboardData(CommandContext& ctx, const QByteArray& bytes)
{
    if (ClipboardHook* cb = ctx.clipboard())
        cb->setData(bytes);
    else
        processClipboardBuffer() = bytes;
}

// COPYCLIP serializes the selection to the clipboard; CUTCLIP also removes
// the originals in one journaled transaction. Pickfirst or prompted, same
// selection stage as the MOVE/COPY family.
class CopyClipCommand : public Command {
public:
    explicit CopyClipCommand(bool cut) : m_cut(cut) {}
    const char* name() const override { return m_cut ? "CUTCLIP" : "COPYCLIP"; }

    Step start(CommandContext& ctx) override
    {
        if (!ctx.selection().isEmpty()) {
            const std::vector<EntityId> ids = ctx.selection().ids();
            ctx.selection().clear();
            return apply(ctx, ids);
        }
        return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        switch (v.kind) {
        case InputValue::Kind::Cancel:
            return Step::cancelled();
        case InputValue::Kind::EntitySet:
            return apply(ctx, v.entitySet);
        case InputValue::Kind::EntityRef:
            m_picked.push_back(v.entityRef);
            return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
        case InputValue::Kind::Finish:
            return apply(ctx, m_picked);
        default:
            return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
        }
    }

private:
    Step apply(CommandContext& ctx, const std::vector<EntityId>& ids)
    {
        std::vector<EntityId> live;
        for (const EntityId id : ids)
            if (ctx.doc().entity(id))
                live.push_back(id);
        if (live.empty())
            return Step::cancelled();

        const QByteArray payload = encodeClipboard(ctx.doc(), live);
        if (payload.isEmpty())
            return Step::cancelled();
        writeClipboardData(ctx, payload);

        if (m_cut) {
            TransactionScope tx(ctx.doc(), QStringLiteral("CUTCLIP"));
            for (const EntityId id : live)
                ctx.doc().removeEntity(id);
            tx.commit();
        }
        ctx.info(QStringLiteral("%1 object(s) %2 to clipboard")
                     .arg(live.size())
                     .arg(m_cut ? QStringLiteral("cut") : QStringLiteral("copied")));
        return Step::done();
    }

    bool m_cut;
    std::vector<EntityId> m_picked;
};

// PASTECLIP: insertion point under the cursor (ghost preview), Enter pastes
// at the original coordinates — the cross-document alignment case.
class PasteClipCommand : public Command {
public:
    const char* name() const override { return "PASTECLIP"; }

    Step start(CommandContext& ctx) override
    {
        m_payload = readClipboardData(ctx);
        const ClipboardInfo info = inspectClipboard(m_payload);
        if (!info.valid || info.entities == 0) {
            ctx.info(QStringLiteral("clipboard: nothing VikiCAD can paste"));
            return Step::done();
        }
        m_base = info.base;
        // Materialized once for the previews; the paste itself re-reads the
        // payload so layers/blocks/styles come along.
        m_ghosts = materializeClipboard(m_payload);
        return Step::cont(
            InputKind::Point,
            QStringLiteral("Specify insertion point <Enter = original position>:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        switch (v.kind) {
        case InputValue::Kind::Cancel:
            return Step::cancelled();
        case InputValue::Kind::Point:
            ctx.setLastPoint(v.point);
            return paste(ctx, v.point - m_base);
        case InputValue::Kind::Finish:
            return paste(ctx, Vec2d{0.0, 0.0});
        case InputValue::Kind::Keyword: {
            // The insertion point is an OPTIONAL stage: a foreign token takes
            // the default (paste in place) and starts the next command —
            // .scr semantics, same contract as WORKPLANE's [OFFSET] stage.
            const Step done = paste(ctx, Vec2d{0.0, 0.0});
            return done.state == Step::State::Done ? Step::doneRepush() : done;
        }
        default:
            return Step::cancelled();
        }
    }

    void previewAt(CommandContext& ctx, const Vec2d& cursor, PrimitiveList& out) override
    {
        (void)ctx;
        RenderContext rc; // coarse preview flattening is fine
        rc.chordTolerance = 0.5;
        const Vec2d offset = cursor - m_base;
        for (const auto& e : m_ghosts) {
            auto ghost = e->clone();
            ghost->transform(Xform2d::translation(offset));
            ghost->buildPrimitives(rc, out);
        }
    }

    bool preview3d(CommandContext& ctx, const Vec2d& cursor, Preview3d& out) override
    {
        (void)ctx;
        TopoDS_Compound comp;
        BRep_Builder builder;
        builder.MakeCompound(comp);
        int solids = 0;
        gp_Trsf trsf;
        trsf.SetTranslation(gp_Vec(cursor.x - m_base.x, cursor.y - m_base.y, 0.0));
        const TopLoc_Location loc(trsf);
        for (const auto& e : m_ghosts) {
            const auto* solid = dynamic_cast<const SolidEntity*>(e.get());
            if (!solid || solid->shape().IsNull())
                continue;
            builder.Add(comp, solid->shape().Moved(loc));
            ++solids;
        }
        if (solids == 0)
            return false;
        out.shape = comp;
        out.effect = Preview3d::Effect::Add;
        return true;
    }

private:
    Step paste(CommandContext& ctx, const Vec2d& offset)
    {
        TransactionScope tx(ctx.doc(), QStringLiteral("PASTECLIP"));
        PasteStats stats;
        QString error;
        if (!pasteClipboard(ctx.doc(), m_payload, offset, stats, error)) {
            ctx.info(error);
            return Step::cancelled(); // scope rolls the transaction back
        }
        tx.commit();

        // The pasted set becomes the selection, ready for an immediate MOVE.
        ctx.selection().clear();
        for (const EntityId id : stats.ids)
            ctx.selection().add(id);

        QString msg = QStringLiteral("%1 object(s) pasted").arg(stats.entities);
        if (stats.layersCreated > 0)
            msg += QStringLiteral(", %1 layer(s) created").arg(stats.layersCreated);
        if (stats.skipped > 0)
            msg += QStringLiteral(", %1 skipped (unknown type)").arg(stats.skipped);
        ctx.info(msg);
        return Step::done();
    }

    QByteArray m_payload;
    Vec2d m_base;
    std::vector<std::unique_ptr<Entity>> m_ghosts;
};

std::unique_ptr<Command> makeCopyClip() { return std::make_unique<CopyClipCommand>(false); }
std::unique_ptr<Command> makeCutClip() { return std::make_unique<CopyClipCommand>(true); }
std::unique_ptr<Command> makePasteClip() { return std::make_unique<PasteClipCommand>(); }

} // namespace

void registerClipboardCommands(CommandProcessor& p)
{
    p.registerCommand(&makeCopyClip);
    p.registerCommand(&makeCutClip);
    p.registerCommand(&makePasteClip);
}

} // namespace viki
