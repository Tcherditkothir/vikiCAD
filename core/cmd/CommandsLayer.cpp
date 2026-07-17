#include "CommandProcessor.h"

#include <algorithm>
#include <cmath>

#include "doc/GerberRole.h"

// G2 layer-stack commands (headless/GUI parity through the single
// CommandProcessor, like everything else):
//
//   LAYER <name> ALPHA <0-100>   compositing opacity (100 = opaque)
//   LAYER <name> RANK <n>        paint rank (lower paints first)
//   LAYER <name> ROLE <token>    reassign the CAM role (recolors + reranks)
//   LAYER <name> UP|DOWN         move one slot in the paint order
//
//   BOARDVIEW TOP|BOTTOM|ALL     CAM stack presets: TOP = bottom-side layers
//                                dimmed; BOTTOM = top-side dimmed + X-mirrored
//                                view (solder side); ALL = everything opaque,
//                                mirror off.
//
// Layer edits are direct (not journaled), consistent with every other layer
// mutation (LayerPanel, importers) — see the Document v1 choice.

namespace viki {
namespace {

// Alpha applied to the "far side" layers by the BOARDVIEW presets.
constexpr int kBoardViewDimAlpha = 25;

enum class BoardSide { Top, Bottom, Always, Neutral };

// Which side of the board a layer belongs to, for the BOARDVIEW presets.
// The reassignable gerberRole prevails; sideless roles (Mask/Silk/Paste/
// Mech) and role-less layers fall back to the importer naming convention.
BoardSide sideOfLayer(const Layer& l)
{
    const QString role =
        l.gerberRole.isEmpty() ? gerberRoleForLayerName(l.name) : l.gerberRole;
    if (role.compare(QLatin1String("Outline"), Qt::CaseInsensitive) == 0 ||
        role.compare(QLatin1String("Drill"), Qt::CaseInsensitive) == 0)
        return BoardSide::Always; // contour + drills stay visible in every view
    if (role.compare(QLatin1String("Copper-Top"), Qt::CaseInsensitive) == 0)
        return BoardSide::Top;
    if (role.compare(QLatin1String("Copper-Bottom"), Qt::CaseInsensitive) == 0)
        return BoardSide::Bottom;
    const QString n = l.name.toUpper();
    if (n.startsWith(QLatin1String("TOP")))
        return BoardSide::Top;
    if (n.startsWith(QLatin1String("BOTTOM")) || n.startsWith(QLatin1String("BOT-")))
        return BoardSide::Bottom;
    return BoardSide::Neutral;
}

QString roleListPrompt()
{
    QStringList tokens;
    for (const GerberRoleSpec& s : gerberRoleSpecs())
        tokens << s.token;
    tokens << QStringLiteral("None");
    return QStringLiteral("Role [%1]:").arg(tokens.join(QLatin1Char('/')));
}

class LayerCommand : public Command {
public:
    const char* name() const override { return "LAYER"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Keyword, QStringLiteral("Layer name:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        switch (m_st) {
        case St::Name: {
            if (v.kind != InputValue::Kind::Keyword &&
                v.kind != InputValue::Kind::Text)
                return Step::cancelled();
            Layer* l = ctx.doc().layerByName(v.text); // case-insensitive
            if (!l) {
                ctx.info(QStringLiteral("no layer named '%1'").arg(v.text));
                return Step::cancelled();
            }
            m_layerId = l->id;
            m_layerName = l->name;
            m_st = St::Option;
            return Step::cont(InputKind::Keyword,
                              QStringLiteral("Option [Alpha/Rank/Role/Up/Down]:"));
        }
        case St::Option: {
            if (v.kind != InputValue::Kind::Keyword) {
                ctx.info(QStringLiteral("expected an option: Alpha, Rank, Role, "
                                        "Up or Down"));
                return Step::cancelled();
            }
            const QString o = v.text; // already uppercased
            if (o == QLatin1String("A") || o == QLatin1String("ALPHA")) {
                m_st = St::AlphaValue;
                return Step::cont(InputKind::Number,
                                  QStringLiteral("Alpha %% (0 = invisible, "
                                                 "100 = opaque) <100>:"));
            }
            if (o == QLatin1String("RANK")) {
                m_st = St::RankValue;
                return Step::cont(InputKind::Number,
                                  QStringLiteral("Paint rank (lower paints "
                                                 "first):"));
            }
            if (o == QLatin1String("ROLE")) {
                m_st = St::RoleValue;
                return Step::cont(InputKind::Keyword, roleListPrompt());
            }
            if (o == QLatin1String("U") || o == QLatin1String("UP"))
                return move(ctx, +1);
            if (o == QLatin1String("D") || o == QLatin1String("DOWN"))
                return move(ctx, -1);
            ctx.info(QStringLiteral("unknown option: %1 (try Alpha, Rank, "
                                    "Role, Up or Down)").arg(o));
            return Step::cancelled();
        }
        case St::AlphaValue: {
            int alpha = 100; // Enter accepts the opaque default
            if (v.kind == InputValue::Kind::Number)
                alpha = std::clamp(int(std::lround(v.number)), 0, 100);
            else if (v.kind != InputValue::Kind::Finish)
                return Step::cancelled();
            ctx.doc().setLayerAlpha(m_layerId, alpha);
            ctx.info(QStringLiteral("layer '%1' alpha = %2%")
                         .arg(m_layerName).arg(alpha));
            return Step::done();
        }
        case St::RankValue: {
            if (v.kind != InputValue::Kind::Number) {
                ctx.info(QStringLiteral("rank unchanged (a number is required)"));
                return Step::cancelled();
            }
            const int rank = int(std::lround(v.number));
            ctx.doc().setLayerRank(m_layerId, rank);
            ctx.info(QStringLiteral("layer '%1' rank = %2 (lower paints first)")
                         .arg(m_layerName).arg(rank));
            return Step::done();
        }
        case St::RoleValue: {
            if (v.kind != InputValue::Kind::Keyword) {
                ctx.info(QStringLiteral("role unchanged"));
                return Step::cancelled();
            }
            if (v.text.compare(QLatin1String("NONE"), Qt::CaseInsensitive) == 0) {
                applyGerberRole(ctx.doc(), m_layerId, QString());
                ctx.info(QStringLiteral("layer '%1' role cleared")
                             .arg(m_layerName));
                return Step::done();
            }
            const GerberRoleSpec* spec = findGerberRole(v.text);
            if (!spec) {
                ctx.info(QStringLiteral("unknown role: %1").arg(v.text));
                return Step::cont(InputKind::Keyword, roleListPrompt());
            }
            applyGerberRole(ctx.doc(), m_layerId, spec->token);
            ctx.info(QStringLiteral("layer '%1' role = %2 (color #%3, rank %4)")
                         .arg(m_layerName, spec->token,
                              QString::number(spec->rgb, 16).rightJustified(
                                  6, QLatin1Char('0')))
                         .arg(spec->rank));
            return Step::done();
        }
        }
        return Step::cancelled();
    }

private:
    Step move(CommandContext& ctx, int delta)
    {
        if (ctx.doc().moveLayerPaintOrder(m_layerId, delta))
            ctx.info(QStringLiteral("layer '%1' moved %2")
                         .arg(m_layerName,
                              delta > 0
                                  ? QStringLiteral("up (painted later, on top)")
                                  : QStringLiteral("down (painted earlier, "
                                                   "below)")));
        else
            ctx.info(QStringLiteral("layer '%1' is already at the %2 of the "
                                    "stack")
                         .arg(m_layerName, delta > 0 ? QStringLiteral("top")
                                                     : QStringLiteral("bottom")));
        return Step::done();
    }

    enum class St { Name, Option, AlphaValue, RankValue, RoleValue };
    St m_st = St::Name;
    LayerId m_layerId = 0;
    QString m_layerName;
};

class BoardViewCommand : public Command {
public:
    const char* name() const override { return "BOARDVIEW"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Keyword,
                          QStringLiteral("Board view [Top/Bottom/All] <All>:"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel)
            return Step::cancelled();
        QString mode = QStringLiteral("ALL");
        if (v.kind == InputValue::Kind::Keyword)
            mode = v.text;
        else if (v.kind != InputValue::Kind::Finish)
            return Step::cancelled();
        if (mode == QLatin1String("T"))
            mode = QStringLiteral("TOP");
        if (mode == QLatin1String("B"))
            mode = QStringLiteral("BOTTOM");
        if (mode == QLatin1String("A"))
            mode = QStringLiteral("ALL");
        if (mode != QLatin1String("TOP") && mode != QLatin1String("BOTTOM") &&
            mode != QLatin1String("ALL")) {
            ctx.info(QStringLiteral("unknown board view: %1 (try TOP, BOTTOM "
                                    "or ALL)").arg(mode));
            return Step::cancelled();
        }

        const BoardSide dimSide = mode == QLatin1String("TOP")
                                      ? BoardSide::Bottom
                                      : BoardSide::Top; // unused for ALL
        int dimmed = 0;
        for (const Layer& l : ctx.doc().layers()) {
            int alpha = 100;
            if (mode != QLatin1String("ALL") && sideOfLayer(l) == dimSide) {
                alpha = kBoardViewDimAlpha;
                ++dimmed;
            }
            if (l.alpha != alpha)
                ctx.doc().setLayerAlpha(l.id, alpha);
        }

        const bool mirror = mode == QLatin1String("BOTTOM");
        if (ctx.view())
            ctx.view()->setMirroredX(mirror);

        if (mode == QLatin1String("ALL")) {
            ctx.info(QStringLiteral("board view ALL: every layer opaque, "
                                    "mirror off"));
        } else {
            ctx.info(QStringLiteral("board view %1: %2 %3-side layer(s) "
                                    "dimmed to %4%")
                         .arg(mode)
                         .arg(dimmed)
                         .arg(mode == QLatin1String("TOP")
                                  ? QStringLiteral("bottom")
                                  : QStringLiteral("top"))
                         .arg(kBoardViewDimAlpha));
            if (mirror)
                ctx.info(ctx.view()
                             ? QStringLiteral("view mirrored left-right "
                                              "(solder side)")
                             : QStringLiteral("no view attached: mirror not "
                                              "applied (headless)"));
        }
        return Step::done();
    }
};

template <typename T>
std::unique_ptr<Command> make()
{
    return std::make_unique<T>();
}

} // namespace

void registerLayerCommands(CommandProcessor& p)
{
    p.registerCommand(&make<LayerCommand>, {QStringLiteral("LA")});
    p.registerCommand(&make<BoardViewCommand>, {QStringLiteral("BV")});
}

} // namespace viki
