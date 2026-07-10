#include "CommandProcessor.h"

#include "solid/SolidOps.h"

// SKETCH [New/Open/Close/List] — sketches as first-class lightweight
// references (a name + a work plane + the 2D entities drawn while it was
// open). New captures the CURRENT work plane (which sketch-on-face already
// sets from the picked face) and opens the sketch; Open restores that plane
// (+ reference snap points from the sketch's entities) and re-opens it;
// Close just closes (the work plane stays). Solids built from a sketch keep
// NO dependency back to it — see EXTRUDE/REVOLVE: tagged profiles are kept.

namespace viki {
namespace {

class SketchCommand : public Command {
public:
    const char* name() const override { return "SKETCH"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Keyword,
                          QStringLiteral("Sketch [New/Open/Close/List] <List>:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_stage) {
        case 0: // action keyword
            if (v.kind == InputValue::Kind::Finish)
                return list(ctx);
            if (v.kind != InputValue::Kind::Keyword)
                return Step::cancelled();
            {
                const QString k = v.text.toUpper();
                if (k == QLatin1String("NEW") || k == QLatin1String("N")) {
                    m_stage = 1;
                    return Step::cont(InputKind::Text,
                                      QStringLiteral("Sketch name <auto>:"));
                }
                if (k == QLatin1String("OPEN") || k == QLatin1String("O")) {
                    m_stage = 2;
                    return Step::cont(InputKind::Text,
                                      QStringLiteral("Sketch name or id:"));
                }
                if (k == QLatin1String("CLOSE") || k == QLatin1String("C"))
                    return close(ctx);
                if (k == QLatin1String("LIST") || k == QLatin1String("L"))
                    return list(ctx);
            }
            ctx.info(QStringLiteral("expected New, Open, Close or List"));
            return Step::cancelled();
        case 1: // New <name>
            if (v.kind == InputValue::Kind::Text)
                return create(ctx, v.text.trimmed());
            if (v.kind == InputValue::Kind::Finish)
                return create(ctx, QString()); // auto-name
            return Step::cancelled();
        case 2: // Open <name-or-id>
            if (v.kind != InputValue::Kind::Text || v.text.trimmed().isEmpty()) {
                ctx.info(QStringLiteral("open which sketch? (name or id)"));
                return Step::cancelled();
            }
            return open(ctx, v.text.trimmed());
        default:
            return Step::cancelled();
        }
    }

private:
    Step create(CommandContext& ctx, QString name)
    {
        Document& doc = ctx.doc();
        if (name.isEmpty()) { // first free "Sketch N"
            int n = int(doc.sketches().size()) + 1;
            do
                name = QStringLiteral("Sketch %1").arg(n++);
            while (doc.sketchByName(name));
        }
        const int64_t id = doc.createSketch(name, documentWorkplane(doc));
        if (id == 0) {
            ctx.info(QStringLiteral("sketch '%1' already exists — SKETCH Open it")
                         .arg(name));
            return Step::cancelled();
        }
        doc.setActiveSketch(id);
        ctx.info(QStringLiteral("sketch '%1' opened (id %2) — entities you draw "
                                "now belong to it; SKETCH Close to finish")
                     .arg(name)
                     .arg(id));
        return Step::done();
    }

    Step open(CommandContext& ctx, const QString& ref)
    {
        Document& doc = ctx.doc();
        const SketchInfo* info = nullptr;
        bool isId = !ref.isEmpty();
        for (const QChar c : ref)
            isId = isId && c.isDigit();
        if (isId)
            info = doc.sketchById(ref.toLongLong());
        if (!info)
            info = doc.sketchByName(ref);
        if (!info) {
            ctx.info(QStringLiteral("no such sketch: %1 (SKETCH List)").arg(ref));
            return Step::cancelled();
        }
        // Restore the sketch's work plane, then rebuild reference snap points
        // from its entities so the profile can pick up where it left off.
        documentWorkplane(doc) = info->plane;
        std::vector<SnapPoint> snaps;
        for (const EntityId id : doc.sketchEntities(info->id))
            if (const Entity* e = doc.entity(id))
                e->snapPoints(snaps);
        doc.setExtraSnapPoints(std::move(snaps));
        doc.setActiveSketch(info->id);
        ctx.info(QStringLiteral("sketch '%1' opened (id %2, %3 entities) — "
                                "SKETCH Close to finish")
                     .arg(info->name)
                     .arg(info->id)
                     .arg(doc.sketchEntities(info->id).size()));
        return Step::done();
    }

    Step close(CommandContext& ctx)
    {
        Document& doc = ctx.doc();
        if (doc.activeSketch() == 0) {
            ctx.info(QStringLiteral("no sketch is open"));
            return Step::done();
        }
        const SketchInfo* info = doc.sketchById(doc.activeSketch());
        doc.setActiveSketch(0);
        doc.clearExtraSnapPoints(); // drop the sketch's reference snaps
        ctx.info(QStringLiteral("sketch '%1' closed (work plane kept)")
                     .arg(info ? info->name : QStringLiteral("?")));
        return Step::done();
    }

    Step list(CommandContext& ctx)
    {
        const Document& doc = ctx.doc();
        if (doc.sketches().empty()) {
            ctx.info(QStringLiteral("no sketches — SKETCH New <name> starts one"));
            return Step::done();
        }
        for (const SketchInfo& s : doc.sketches())
            ctx.info(QStringLiteral("%1 %2 '%3' — %4 entities")
                         .arg(s.id == doc.activeSketch() ? QStringLiteral("*")
                                                         : QStringLiteral(" "))
                         .arg(s.id)
                         .arg(s.name)
                         .arg(doc.sketchEntities(s.id).size()));
        return Step::done();
    }

    int m_stage = 0;
};

std::unique_ptr<Command> makeSketch() { return std::make_unique<SketchCommand>(); }

} // namespace

void registerSketchCommands(CommandProcessor& p)
{
    p.registerCommand(&makeSketch, {QStringLiteral("SK")});
}

} // namespace viki
