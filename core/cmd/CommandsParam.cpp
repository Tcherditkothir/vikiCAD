#include "CommandProcessor.h"

#include "doc/Document.h"

// PARAM name expr — define or update a user parameter. Named values with
// expressions that may reference other parameters (d = 10, w = 2*d). The
// whole table re-evaluates on every change so dependents track their inputs.
// This is groundwork for driving dimensions/features later.
//
// Grammar note (per project rule): the command takes a single free-text line
// "name expr" so the expression (which has no spaces of its own, e.g. 2*d) is
// not eaten by the numeric/EntitySet greedy stages. "PARAM w 2*d" works in one
// line; interactively, PARAM prompts once for "name expr".

namespace viki {
namespace {

class ParamCommand : public Command {
public:
    const char* name() const override { return "PARAM"; }

    Step start(CommandContext&) override
    {
        return Step::cont(InputKind::Text,
                          QStringLiteral("Parameter (name expr, e.g. w 2*d):"));
    }

    Step onInput(CommandContext& ctx, const InputValue& v) override
    {
        if (v.kind == InputValue::Kind::Cancel || v.kind == InputValue::Kind::Finish)
            return Step::cancelled();
        if (v.kind != InputValue::Kind::Text)
            return Step::cancelled();

        const QString line = v.text.trimmed();
        const int sp = firstSpace(line);
        if (sp < 0) {
            // Just a name: report its current value.
            const Param* p = ctx.doc().params().find(line);
            if (!p) {
                ctx.info(QStringLiteral("no such parameter: %1").arg(line));
                return Step::cancelled();
            }
            ctx.info(reportLine(*p));
            return Step::done();
        }

        const QString name = line.left(sp);
        const QString expr = line.mid(sp + 1).trimmed();
        if (!ParamTable::isValidName(name)) {
            ctx.info(QStringLiteral("invalid parameter name: %1").arg(name));
            return Step::cancelled();
        }

        const bool ok = ctx.doc().params().set(name, expr);
        const Param* p = ctx.doc().params().find(name);
        if (ok && p) {
            ctx.info(reportLine(*p));
        } else {
            ctx.info(QStringLiteral("%1 = %2  (unresolved: unknown reference, "
                                    "cycle, or syntax error)")
                         .arg(name, expr));
        }
        return Step::done();
    }

private:
    static int firstSpace(const QString& s)
    {
        for (int i = 0; i < s.size(); ++i)
            if (s.at(i).isSpace())
                return i;
        return -1;
    }

    static QString reportLine(const Param& p)
    {
        if (!p.valid)
            return QStringLiteral("%1 = %2  (unresolved)").arg(p.name, p.expr);
        return QStringLiteral("%1 = %2  ->  %3")
            .arg(p.name, p.expr)
            .arg(p.value, 0, 'g', 12);
    }
};

std::unique_ptr<Command> makeParam() { return std::make_unique<ParamCommand>(); }

} // namespace

void registerParamCommands(CommandProcessor& p)
{
    p.registerCommand(&makeParam, {QStringLiteral("PARAMETER")});
}

} // namespace viki
