#include "CommandProcessor.h"

#include <QDateTime>

#include "doc/EntitiesEx.h"
#include "doc/Entities.h"
#include "doc/StickyNote.h"
#include "geom/GearGeometry.h"

// GEAR: generate an involute spur-gear tooth profile from module / teeth /
// pressure angle, plus a sticky note recording the computed dimensions AND
// the design reasoning (tooth count vs strength, module, pressure angle,
// speed/power, meshing). Lex asked for "un vrai outil de dessins de spur
// gears" — a real design aid, not just a ratio calculator.

namespace viki {
namespace {

QString num(double v, int dec = 3)
{
    return QString::number(v, 'f', dec);
}

QString designNote(const gear::GearParams& p, const gear::GearMetrics& g)
{
    const int minZ = int(std::ceil(g.minTeethNoUndercut));
    QString s;
    s += QStringLiteral("**Spur gear** — m=%1  z=%2  PA=%3°\n")
             .arg(num(p.module, 3))
             .arg(p.teeth)
             .arg(num(p.pressureAngleDeg, 1));
    s += QStringLiteral("\n**Dimensions (mm)**\n");
    s += QStringLiteral("- Pitch Ø: %1   (d = m·z)\n").arg(num(g.pitchDiameter));
    s += QStringLiteral("- Base Ø: %1   (d·cos α)\n").arg(num(g.baseDiameter));
    s += QStringLiteral("- Outside Ø: %1   (d + 2·m)\n").arg(num(g.outsideDiameter));
    s += QStringLiteral("- Root Ø: %1   (d − 2.5·m)\n").arg(num(g.rootDiameter));
    s += QStringLiteral("- Addendum: %1   Dedendum: %2   Whole depth: %3\n")
             .arg(num(g.addendum)).arg(num(g.dedendum)).arg(num(g.wholeDepth));
    s += QStringLiteral("- Circular pitch: %1   Tooth thickness @pitch: %2\n")
             .arg(num(g.circularPitch)).arg(num(g.toothThickness));

    s += QStringLiteral("\n**To mesh with a second gear**\n");
    s += QStringLiteral("- It MUST share the same module and pressure angle.\n");
    s += QStringLiteral("- Ratio i = z₂/z₁ (speed ↓, torque ↑ by i).\n");
    s += QStringLiteral("- Centre distance = m·(z₁+z₂)/2.\n");

    s += QStringLiteral("\n**Design reasoning**\n");
    s += QStringLiteral("• Tooth count. Min without undercut ≈ 2/sin²α = %1 "
                        "teeth. ")
             .arg(minZ);
    if (g.undercut)
        s += QStringLiteral("⚠ z=%1 is BELOW this — the root is undercut "
                            "(weaker, thinner). Raise z, use a larger PA, or "
                            "apply profile shift (+x).\n")
                 .arg(p.teeth);
    else
        s += QStringLiteral("z=%1 is fine. Fewer teeth = more compact but "
                            "higher tooth load & less smooth; more teeth = "
                            "smoother, quieter, bigger, thinner teeth.\n")
                 .arg(p.teeth);
    s += QStringLiteral("• Module. It sets tooth SIZE and load capacity. "
                        "Bigger m → stronger, coarser, noisier; smaller m → "
                        "finer, quieter, less load. Pick m from the load first, "
                        "then z for the size.\n");
    s += QStringLiteral("• Pressure angle. 20° is the modern default (good "
                        "strength/contact balance). 14.5°: higher contact "
                        "ratio, quieter, weaker teeth, higher min-teeth (~32). "
                        "25°: stronger teeth & less undercut, but higher "
                        "bearing loads and lower contact ratio.\n");
    s += QStringLiteral("• Face width (into the page). Rule of thumb "
                        "b ≈ 8–12·m = %1–%2 mm; wider carries more power but "
                        "needs good alignment.\n")
             .arg(num(8 * p.module, 1)).arg(num(12 * p.module, 1));
    s += QStringLiteral("• Speed & power. Transmitted torque T = P/ω, "
                        "tangential force Fₜ = 2T/d = 2T/%1 mm. Size m & b so "
                        "Fₜ passes bending (Lewis) and surface (Hertz) limits. "
                        "High speed → finer pitch + better accuracy/quality to "
                        "tame dynamic loads; high power → bigger m, wider b, or "
                        "harder material.\n")
             .arg(num(g.pitchDiameter, 1));
    return s;
}

class GearCommand : public Command {
public:
    const char* name() const override { return "GEAR"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Number, QStringLiteral("Module (mm) <2>:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_stage) {
        case 0:
            if (v.kind == InputValue::Kind::Number && v.number > 0)
                m_p.module = v.number;
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_stage = 1;
            return Step::cont(InputKind::Number,
                              QStringLiteral("Number of teeth <20>:"));
        case 1:
            if (v.kind == InputValue::Kind::Number && v.number >= 3)
                m_p.teeth = int(std::lround(v.number));
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_stage = 2;
            return Step::cont(InputKind::Number,
                              QStringLiteral("Pressure angle (deg) <20>:"));
        case 2:
            if (v.kind == InputValue::Kind::Number && v.number > 0 && v.number < 45)
                m_p.pressureAngleDeg = v.number;
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_stage = 3;
            return Step::cont(InputKind::Number,
                              QStringLiteral("Bore diameter <0=none>:"));
        case 3:
            if (v.kind == InputValue::Kind::Number && v.number >= 0)
                m_p.boreDiameter = v.number;
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            m_stage = 4;
            return Step::cont(InputKind::Point, QStringLiteral("Center point:"));
        case 4: {
            if (v.kind != InputValue::Kind::Point)
                return Step::cancelled();
            const Vec2d center = v.point;
            const gear::GearMetrics g = gear::metrics(m_p);
            const std::vector<Vec2d> pts = gear::profile(m_p, center, 0.03);
            if (pts.size() < 8)
                return Step::cancelled();

            std::vector<PolyVertex> verts;
            verts.reserve(pts.size());
            for (const Vec2d& pt : pts)
                verts.push_back({pt, 0.0});

            ctx.doc().beginTransaction(QStringLiteral("GEAR"));
            ctx.doc().addEntity(
                std::make_unique<PolylineEntity>(std::move(verts), true));
            if (m_p.boreDiameter > 0.0)
                ctx.doc().addEntity(std::make_unique<CircleEntity>(
                    center, m_p.boreDiameter / 2.0));

            // Design note anchored just outside the tip circle.
            auto note = std::make_unique<StickyNoteEntity>();
            note->text = designNote(m_p, g);
            note->anchor = center + Vec2d{g.outsideDiameter / 2.0 + m_p.module,
                                          g.outsideDiameter / 2.0};
            note->created = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            note->modified = note->created;
            const LayerId nl = ctx.doc().ensureLayer(
                QLatin1String(StickyNoteEntity::kLayerName), 0xE8C84A);
            ctx.doc().setLayerPrintable(nl, false);
            note->setLayerId(nl);
            ctx.doc().addEntity(std::move(note));
            ctx.doc().commitTransaction();

            if (g.undercut)
                ctx.info(QStringLiteral("gear made — note: z=%1 undercuts "
                                        "(min ~%2); see the design note")
                             .arg(m_p.teeth)
                             .arg(int(std::ceil(g.minTeethNoUndercut))));
            else
                ctx.info(QStringLiteral("gear made — pitch Ø %1 mm, see note")
                             .arg(num(g.pitchDiameter, 2)));
            return Step::done();
        }
        default:
            return Step::cancelled();
        }
    }

private:
    int m_stage = 0;
    gear::GearParams m_p;
};

std::unique_ptr<Command> makeGear() { return std::make_unique<GearCommand>(); }

} // namespace

void registerGearCommands(CommandProcessor& p)
{
    p.registerCommand(&makeGear, {QStringLiteral("SPURGEAR")});
}

} // namespace viki
