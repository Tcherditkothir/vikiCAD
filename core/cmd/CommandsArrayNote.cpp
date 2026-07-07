#include <QDateTime>

#include "CommandProcessor.h"

#include "doc/ArrayEntity.h"
#include "doc/StickyNote.h"
#include "render/HitTest.h"

// M5: associative arrays and sticky notes.

namespace viki {
namespace {

QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

QString userName()
{
    const QByteArray user = qgetenv("USER");
    return user.isEmpty() ? QStringLiteral("unknown") : QString::fromLocal8Bit(user);
}

// Shared: parameters FIRST, then the entity set (params first so the
// EntitySet token gulp cannot swallow numeric parameters in scripts).
class ArrayCommandBase : public Command {
public:
    Step start(CommandContext& ctx) override
    {
        if (!ctx.selection().isEmpty()) {
            m_ids = ctx.selection().ids();
            ctx.selection().clear();
        }
        return firstParam(ctx);
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (!m_paramsDone)
            return onParam(ctx, v);
        switch (v.kind) {
        case InputValue::Kind::EntitySet:
            m_ids = v.entitySet;
            return finishWithIds(ctx);
        case InputValue::Kind::EntityRef:
            m_ids.push_back(v.entityRef);
            return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
        case InputValue::Kind::Finish:
            if (m_ids.empty())
                return Step::cancelled();
            return finishWithIds(ctx);
        default:
            return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
        }
    }

protected:
    virtual Step firstParam(CommandContext& ctx) = 0;
    virtual Step onParam(CommandContext& ctx, const InputValue& v) = 0;
    virtual Step finishWithIds(CommandContext& ctx) = 0;

    // Called by subclasses when their last parameter arrived.
    Step selectStage(CommandContext& ctx)
    {
        m_paramsDone = true;
        if (!m_ids.empty())
            return finishWithIds(ctx); // pickfirst selection
        return Step::cont(InputKind::EntitySet, QStringLiteral("Select objects:"));
    }

    Step commitArray(CommandContext& ctx, std::unique_ptr<ArrayEntity> array)
    {
        ctx.doc().beginTransaction(QLatin1String(name()));
        for (const EntityId id : m_ids) {
            if (const Entity* e = ctx.doc().entity(id)) {
                array->prototypes.push_back(e->clone());
                ctx.doc().removeEntity(id);
            }
        }
        if (array->prototypes.empty()) {
            ctx.doc().rollbackTransaction();
            return Step::cancelled();
        }
        ctx.doc().addEntity(std::move(array));
        ctx.doc().commitTransaction();
        return Step::done();
    }

    std::vector<EntityId> m_ids;
    bool m_paramsDone = false;
};

// ARRAYRECT ids rows cols col_spacing row_spacing
class ArrayRectCommand : public ArrayCommandBase {
public:
    const char* name() const override { return "ARRAYRECT"; }

protected:
    Step firstParam(CommandContext&) override
    {
        return Step::cont(InputKind::Number, QStringLiteral("Number of rows:"));
    }

    Step onParam(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind != InputValue::Kind::Number)
            return Step::cancelled();
        switch (m_stage++) {
        case 0:
            m_rows = std::max(1, int(v.number));
            return Step::cont(InputKind::Number, QStringLiteral("Number of columns:"));
        case 1:
            m_cols = std::max(1, int(v.number));
            return Step::cont(InputKind::Number, QStringLiteral("Column spacing:"));
        case 2:
            m_colSpacing = v.number;
            return Step::cont(InputKind::Number, QStringLiteral("Row spacing:"));
        case 3:
            m_rowSpacing = v.number;
            return selectStage(ctx);
        default:
            return Step::cancelled();
        }
    }

    Step finishWithIds(CommandContext& ctx) override
    {
        auto arr = std::make_unique<ArrayEntity>();
        arr->mode = ArrayEntity::Mode::Rectangular;
        arr->rows = m_rows;
        arr->cols = m_cols;
        arr->colSpacing = m_colSpacing;
        arr->rowSpacing = m_rowSpacing;
        return commitArray(ctx, std::move(arr));
    }

private:
    int m_stage = 0;
    int m_rows = 2, m_cols = 2;
    double m_colSpacing = 10, m_rowSpacing = 10;
};

// ARRAYPOLAR ids center count angle_span_deg
class ArrayPolarCommand : public ArrayCommandBase {
public:
    const char* name() const override { return "ARRAYPOLAR"; }

protected:
    Step firstParam(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Center of array:"));
    }

    Step onParam(CommandContext& ctx, const InputValue& v) override
    {
        switch (m_stage) {
        case 0:
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            m_center = v.point;
            ++m_stage;
            return Step::cont(InputKind::Number, QStringLiteral("Number of items:"));
        case 1:
            if (v.kind != InputValue::Kind::Number)
                return Step::cancelled();
            m_count = std::max(1, int(v.number));
            ++m_stage;
            return Step::cont(InputKind::Number,
                              QStringLiteral("Angle span (degrees) <360>:"));
        case 2:
            if (v.kind == InputValue::Kind::Number)
                m_span = std::clamp(v.number, 1.0, 360.0) * M_PI / 180.0;
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            return selectStage(ctx);
        default:
            return Step::cancelled();
        }
    }

    Step finishWithIds(CommandContext& ctx) override
    {
        auto arr = std::make_unique<ArrayEntity>();
        arr->mode = ArrayEntity::Mode::Polar;
        arr->center = m_center;
        arr->count = m_count;
        arr->angleSpan = m_span;
        return commitArray(ctx, std::move(arr));
    }

private:
    int m_stage = 0;
    Vec2d m_center;
    int m_count = 6;
    double m_span = 2.0 * M_PI;
};

// ARRAYEDIT pick key value ... — live parameter editing (journaled).
class ArrayEditCommand : public Command {
public:
    const char* name() const override { return "ARRAYEDIT"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Pick an array:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (m_id == kInvalidEntityId) {
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            const EntityId id = hittest::pick(ctx.doc(), v.point, ctx.pickTolerance());
            if (!dynamic_cast<ArrayEntity*>(ctx.doc().entity(id))) {
                ctx.info(QStringLiteral("that is not an array"));
                return Step::cont(InputKind::Point, QStringLiteral("Pick an array:"));
            }
            m_id = id;
            return keyPrompt();
        }
        if (m_pendingKey.isEmpty()) {
            if (v.kind == InputValue::Kind::Finish)
                return Step::done();
            if (v.kind != InputValue::Kind::Keyword)
                return Step::cancelled();
            m_pendingKey = v.text;
            return Step::cont(InputKind::Number,
                              QStringLiteral("Value for %1:").arg(m_pendingKey));
        }
        if (v.kind != InputValue::Kind::Number)
            return Step::cancelled();
        ctx.doc().beginTransaction(QStringLiteral("ARRAYEDIT"));
        auto* arr = dynamic_cast<ArrayEntity*>(ctx.doc().beginModify(m_id));
        if (arr) {
            const QString k = m_pendingKey;
            if (k == QLatin1String("ROWS")) arr->rows = std::max(1, int(v.number));
            else if (k == QLatin1String("COLS")) arr->cols = std::max(1, int(v.number));
            else if (k == QLatin1String("DX")) arr->colSpacing = v.number;
            else if (k == QLatin1String("DY")) arr->rowSpacing = v.number;
            else if (k == QLatin1String("COUNT")) arr->count = std::max(1, int(v.number));
            else if (k == QLatin1String("SPAN"))
                arr->angleSpan = std::clamp(v.number, 1.0, 360.0) * M_PI / 180.0;
            else if (k == QLatin1String("SUPPRESS")) arr->suppressed.insert(int(v.number));
            else if (k == QLatin1String("RESTORE")) arr->suppressed.erase(int(v.number));
            ctx.doc().endModify(m_id);
        }
        ctx.doc().commitTransaction();
        m_pendingKey.clear();
        return keyPrompt();
    }

private:
    Step keyPrompt() const
    {
        return Step::cont(
            InputKind::Keyword,
            QStringLiteral(
                "Set [ROWS/COLS/DX/DY/COUNT/SPAN/SUPPRESS/RESTORE] (Enter to finish):"));
    }
    EntityId m_id = kInvalidEntityId;
    QString m_pendingKey;
};

// NOTE x,y text — free note anchored at a point.
// NOTEPIN pick-point text — note pinned to an entity.
class NoteCommand : public Command {
public:
    explicit NoteCommand(bool pinned) : m_pinned(pinned) {}
    const char* name() const override { return m_pinned ? "NOTEPIN" : "NOTE"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point,
                          m_pinned ? QStringLiteral("Pick the entity to pin the note to:")
                                   : QStringLiteral("Note anchor point:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (m_stage == 0) {
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            if (m_pinned) {
                m_target = hittest::pick(ctx.doc(), v.point, ctx.pickTolerance());
                if (m_target == kInvalidEntityId) {
                    ctx.info(QStringLiteral("nothing there"));
                    return Step::cont(InputKind::Point,
                                      QStringLiteral("Pick the entity to pin to:"));
                }
            }
            m_anchor = v.point;
            m_stage = 1;
            return Step::cont(InputKind::Text, QStringLiteral("Note text (markdown):"));
        }
        if (v.kind != InputValue::Kind::Text || v.text.trimmed().isEmpty())
            return Step::cancelled();

        auto note = std::make_unique<StickyNoteEntity>();
        QString content = v.text;
        content.replace(QLatin1String("\\n"), QLatin1String("\n"));
        note->text = content;
        note->author = userName();
        note->created = nowIso();
        note->modified = note->created;
        note->anchor = m_anchor;
        note->target = m_target;
        const LayerId layer = ctx.doc().ensureLayer(
            QLatin1String(StickyNoteEntity::kLayerName), 0xE8C84A);
        ctx.doc().setLayerPrintable(layer, false); // notes never print by default
        note->setLayerId(layer);
        ctx.doc().beginTransaction(QLatin1String(name()));
        ctx.doc().addEntity(std::move(note));
        ctx.doc().commitTransaction();
        return Step::done();
    }

private:
    bool m_pinned;
    int m_stage = 0;
    Vec2d m_anchor;
    EntityId m_target = kInvalidEntityId;
};

// NOTEEDIT pick new-text
class NoteEditCommand : public Command {
public:
    const char* name() const override { return "NOTEEDIT"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Point, QStringLiteral("Pick the note:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (m_id == kInvalidEntityId) {
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            const EntityId id = hittest::pick(ctx.doc(), v.point, ctx.pickTolerance());
            if (!dynamic_cast<StickyNoteEntity*>(ctx.doc().entity(id))) {
                ctx.info(QStringLiteral("that is not a sticky note"));
                return Step::cont(InputKind::Point, QStringLiteral("Pick the note:"));
            }
            m_id = id;
            return Step::cont(InputKind::Text, QStringLiteral("New text:"));
        }
        if (v.kind != InputValue::Kind::Text)
            return Step::cancelled();
        ctx.doc().beginTransaction(QStringLiteral("NOTEEDIT"));
        if (auto* note = dynamic_cast<StickyNoteEntity*>(ctx.doc().beginModify(m_id))) {
            QString content = v.text;
            content.replace(QLatin1String("\\n"), QLatin1String("\n"));
            note->text = content;
            note->modified = nowIso();
            ctx.doc().endModify(m_id);
        }
        ctx.doc().commitTransaction();
        return Step::done();
    }

private:
    EntityId m_id = kInvalidEntityId;
};

std::unique_ptr<Command> makeNote() { return std::make_unique<NoteCommand>(false); }
std::unique_ptr<Command> makeNotePin() { return std::make_unique<NoteCommand>(true); }

template <typename T>
std::unique_ptr<Command> make()
{
    return std::make_unique<T>();
}

} // namespace

void registerArrayNoteCommands(CommandProcessor& p)
{
    p.registerCommand(&make<ArrayRectCommand>, {QStringLiteral("AR")});
    p.registerCommand(&make<ArrayPolarCommand>, {QStringLiteral("AP")});
    p.registerCommand(&make<ArrayEditCommand>);
    p.registerCommand(&makeNote);
    p.registerCommand(&makeNotePin);
    p.registerCommand(&make<NoteEditCommand>);
}

} // namespace viki
