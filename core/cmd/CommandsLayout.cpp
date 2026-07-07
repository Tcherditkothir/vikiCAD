#include "CommandProcessor.h"

#include "io/PdfPlotter.h"

// M5: LAYOUT (create/update a paper layout with one fitted viewport) and
// PLOT (render a layout to PDF).

namespace viki {
namespace {

struct Paper {
    const char* key;
    double w, h;
};
const Paper kPapers[] = {{"A4L", 297, 210}, {"A4P", 210, 297}, {"A3L", 420, 297},
                         {"A3P", 297, 420}, {"A2L", 594, 420}, {"A1L", 841, 594}};

// LAYOUT name paper scale|FIT
class LayoutCommand : public Command {
public:
    const char* name() const override { return "LAYOUT"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Keyword, QStringLiteral("Layout name:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_stage) {
        case 0:
            if (v.kind != InputValue::Kind::Keyword)
                return Step::cancelled();
            m_name = v.text;
            m_stage = 1;
            return Step::cont(InputKind::Keyword,
                              QStringLiteral("Paper [A4L/A4P/A3L/A3P/A2L/A1L] <A4L>:"));
        case 1: {
            if (v.kind == InputValue::Kind::Keyword) {
                bool found = false;
                for (const Paper& p : kPapers) {
                    if (v.text == QLatin1String(p.key)) {
                        m_w = p.w;
                        m_h = p.h;
                        found = true;
                    }
                }
                if (!found) {
                    ctx.info(QStringLiteral("unknown paper size"));
                    return Step::cont(InputKind::Keyword, QStringLiteral("Paper:"));
                }
            } else if (v.kind != InputValue::Kind::Finish) {
                return Step::cancelled();
            }
            m_stage = 2;
            return Step::cont(InputKind::Keyword,
                              QStringLiteral("Scale (e.g. 1, 0.5, 2) or FIT <FIT>:"));
        }
        case 2: {
            double scale = -1; // FIT
            if (v.kind == InputValue::Kind::Keyword && v.text != QLatin1String("FIT")) {
                bool ok = false;
                const double s = v.text.toDouble(&ok);
                if (ok && s > 0)
                    scale = s;
            }
            Layout* layout = ctx.doc().ensureLayout(m_name);
            layout->paperW = m_w;
            layout->paperH = m_h;
            layout->viewports.clear();
            Viewport vp;
            vp.x = 10;
            vp.y = 10;
            vp.w = m_w - 20;
            vp.h = m_h - 20;
            const BBox2d ext = ctx.doc().extents();
            if (ext.isValid()) {
                vp.center = ext.center();
                if (scale <= 0) {
                    // FIT with 5% headroom.
                    const double sx = vp.w / std::max(ext.width(), 1e-6);
                    const double sy = vp.h / std::max(ext.height(), 1e-6);
                    scale = 0.95 * std::min(sx, sy);
                }
            } else if (scale <= 0) {
                scale = 1.0;
            }
            vp.scale = scale;
            layout->viewports.push_back(vp);
            ctx.info(QStringLiteral("layout '%1' %2x%3 mm, scale %4")
                         .arg(m_name)
                         .arg(m_w)
                         .arg(m_h)
                         .arg(scale, 0, 'g', 4));
            return Step::done();
        }
        default:
            return Step::cancelled();
        }
    }

private:
    int m_stage = 0;
    QString m_name;
    double m_w = 297, m_h = 210;
};

// PLOT layout-name output-path
class PlotCommand : public Command {
public:
    const char* name() const override { return "PLOT"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Keyword, QStringLiteral("Layout name:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        if (m_stage == 0) {
            if (v.kind != InputValue::Kind::Keyword)
                return Step::cancelled();
            if (!ctx.doc().layoutByName(v.text)) {
                ctx.info(QStringLiteral("no layout named '%1' (use LAYOUT first)").arg(v.text));
                return Step::cancelled();
            }
            m_layout = v.text;
            m_stage = 1;
            return Step::cont(InputKind::Text, QStringLiteral("Output PDF path:"));
        }
        if (v.kind != InputValue::Kind::Text || v.text.trimmed().isEmpty())
            return Step::cancelled();
        QString error;
        const Layout* layout = ctx.doc().layoutByName(m_layout);
        if (plotToPdf(ctx.doc(), *layout, v.text.trimmed(), error))
            ctx.info(QStringLiteral("plotted '%1' to %2").arg(m_layout, v.text.trimmed()));
        else
            ctx.info(QStringLiteral("plot failed: %1").arg(error));
        return Step::done();
    }

private:
    int m_stage = 0;
    QString m_layout;
};

template <typename T>
std::unique_ptr<Command> make()
{
    return std::make_unique<T>();
}

} // namespace

void registerLayoutCommands(CommandProcessor& p)
{
    p.registerCommand(&make<LayoutCommand>);
    p.registerCommand(&make<PlotCommand>);
}

} // namespace viki
