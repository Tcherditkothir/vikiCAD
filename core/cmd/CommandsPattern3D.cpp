#include "CommandProcessor.h"

#include <algorithm>

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include "solid/SolidEntity.h"
#include "solid/SolidOps.h"

// 3D PATTERNS (Fusion "Rectangular/Circular Pattern"): PATTERN3D-family
// commands that CLONE a picked solid into N transformed copies. The original
// stays put (its transform is the identity, cell 0) and each additional
// placement becomes a fresh SolidEntity that shares the source's `component`
// and `transparency`. Numeric params come BEFORE the entity selection (the
// EntitySet gulp rule). Placement math lives in solidops::pattern{Rect,Polar}
// and mirrors CommandsAssembly's gp_Trsf usage.

namespace viki {
namespace {

// Trailing entity selection + clone-per-transform, shared by both patterns.
class Pattern3DCommand : public Command {
public:
    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (!m_paramsDone)
            return onParams(ctx, v);
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
    // The placements, INCLUDING the identity for the source at cell 0.
    virtual std::vector<gp_Trsf> placements() const = 0;
    QString selectPrompt() const
    {
        return QStringLiteral("Select solids to pattern (Enter to finish):");
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
        const std::vector<gp_Trsf> ts = placements();
        ctx.doc().beginTransaction(QString::fromLatin1(name()));
        int copies = 0;
        for (const EntityId id : m_ids) {
            const auto* src = dynamic_cast<const SolidEntity*>(ctx.doc().entity(id));
            if (!src)
                continue; // only solids have a 3D placement
            // Skip cell 0 (identity): the source stays where it is; add the
            // rest as fresh clones sharing component + transparency.
            for (std::size_t i = 1; i < ts.size(); ++i) {
                auto clone = std::make_unique<SolidEntity>(src->shape());
                clone->component = src->component;
                clone->transparency = src->transparency;
                clone->setColor(src->color());
                clone->applyTrsf(ts[i]);
                ctx.doc().addEntity(std::move(clone));
                ++copies;
            }
        }
        ctx.doc().commitTransaction();
        ctx.info(QStringLiteral("%1 pattern copy/copies created").arg(copies));
        return Step::done();
    }

    std::vector<EntityId> m_ids;
};

// PATTERN3D RECT: counts nx ny nz then spacings dx dy dz, then the solids.
class PatternRect3DCommand : public Pattern3DCommand {
public:
    const char* name() const override { return "PATTERN3D"; }
    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Number, QStringLiteral("Count X <1>:"));
    }

protected:
    Step onParams(CommandContext&, const InputValue& v) override
    {
        if (v.kind != InputValue::Kind::Number && v.kind != InputValue::Kind::Finish)
            return Step::cancelled();
        const double val = v.kind == InputValue::Kind::Number ? v.number : m_defaults[m_step];
        static const char* prompts[] = {
            "Count Y <1>:",   "Count Z <1>:",   "Spacing X <10>:",
            "Spacing Y <10>:", "Spacing Z <10>:"};
        switch (m_step++) {
        case 0: m_nx = std::max(1, int(val + 0.5)); break;
        case 1: m_ny = std::max(1, int(val + 0.5)); break;
        case 2: m_nz = std::max(1, int(val + 0.5)); break;
        case 3: m_dx = val; break;
        case 4: m_dy = val; break;
        default: m_dz = val; return beginSelection();
        }
        return Step::cont(InputKind::Number, QLatin1String(prompts[m_step - 1]));
    }
    std::vector<gp_Trsf> placements() const override
    {
        return solidops::patternRect(m_nx, m_ny, m_nz, m_dx, m_dy, m_dz);
    }

private:
    int m_step = 0;
    // Defaults per step index (counts default 1, spacings default 10).
    const double m_defaults[6] = {1, 1, 1, 10, 10, 10};
    int m_nx = 1, m_ny = 1, m_nz = 1;
    double m_dx = 10, m_dy = 10, m_dz = 10;
};

// PATTERNPOLAR3D: count, axis X/Y/Z, total angle, center X Y Z, then solids.
class PatternPolar3DCommand : public Pattern3DCommand {
public:
    const char* name() const override { return "PATTERNPOLAR3D"; }
    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Number, QStringLiteral("Count <4>:"));
    }

protected:
    Step onParams(CommandContext&, const InputValue& v) override
    {
        switch (m_step) {
        case 0:
            if (v.kind == InputValue::Kind::Number)
                m_count = std::max(1, int(v.number + 0.5));
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_step = 1;
            return Step::cont(InputKind::Keyword,
                              QStringLiteral("Axis [X/Y/Z] <Z>:"));
        case 1: {
            if (v.kind == InputValue::Kind::Keyword) {
                const QString a = v.text.toUpper();
                if (a == QLatin1String("X"))
                    m_axis = gp_Dir(1, 0, 0);
                else if (a == QLatin1String("Y"))
                    m_axis = gp_Dir(0, 1, 0);
                else
                    m_axis = gp_Dir(0, 0, 1);
            } else if (v.kind != InputValue::Kind::Finish) {
                return Step::cancelled();
            }
            m_step = 2;
            return Step::cont(InputKind::Number,
                              QStringLiteral("Total angle (degrees) <360>:"));
        }
        case 2:
            m_angle = v.kind == InputValue::Kind::Number ? v.number : 360.0;
            if (v.kind != InputValue::Kind::Number && v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_step = 3;
            return Step::cont(InputKind::Number, QStringLiteral("Center X <0>:"));
        case 3:
            m_cx = v.kind == InputValue::Kind::Number ? v.number : 0.0;
            m_step = 4;
            return Step::cont(InputKind::Number, QStringLiteral("Center Y <0>:"));
        case 4:
            m_cy = v.kind == InputValue::Kind::Number ? v.number : 0.0;
            m_step = 5;
            return Step::cont(InputKind::Number, QStringLiteral("Center Z <0>:"));
        default:
            m_cz = v.kind == InputValue::Kind::Number ? v.number : 0.0;
            return beginSelection();
        }
    }
    std::vector<gp_Trsf> placements() const override
    {
        return solidops::patternPolar(m_count, m_axis, gp_Pnt(m_cx, m_cy, m_cz),
                                      m_angle);
    }

private:
    int m_step = 0;
    int m_count = 4;
    gp_Dir m_axis{0, 0, 1};
    double m_angle = 360.0;
    double m_cx = 0, m_cy = 0, m_cz = 0;
};

std::unique_ptr<Command> makePatternRect3D()
{
    return std::make_unique<PatternRect3DCommand>();
}
std::unique_ptr<Command> makePatternPolar3D()
{
    return std::make_unique<PatternPolar3DCommand>();
}

} // namespace

void registerPattern3DCommands(CommandProcessor& p)
{
    p.registerCommand(&makePatternRect3D, {QStringLiteral("P3R")});
    p.registerCommand(&makePatternPolar3D, {QStringLiteral("P3P")});
}

} // namespace viki
