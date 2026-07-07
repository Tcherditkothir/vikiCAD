#include "CommandProcessor.h"

namespace viki {

CommandProcessor::CommandProcessor(CommandContext& ctx)
    : m_ctx(ctx)
{
}

void CommandProcessor::registerCommand(CommandFactory factory, const QStringList& aliases)
{
    const auto probe = factory();
    m_registry[QString::fromLatin1(probe->name()).toUpper()] = factory;
    for (const QString& alias : aliases)
        m_registry[alias.toUpper()] = factory;
}

CommandProcessor::Result CommandProcessor::submit(const QString& line, bool strict)
{
    QStringList tokens = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);

    if (!m_active) {
        if (tokens.isEmpty())
            return {};
        const QString name = tokens.takeFirst().toUpper();
        const auto it = m_registry.find(name);
        if (it == m_registry.end())
            return {false, QStringLiteral("unknown command: %1").arg(name), false, {}};
        m_active = it->second();
        m_pendingTokens = tokens;
        Result r = drive(m_active->start(m_ctx));
        if (!r.ok || !r.pending)
            return r;
        r = feedPendingTokens();
        if (r.ok && r.pending && strict) {
            // Implicit Finishes let repeating/optional stages terminate
            // (LINE points, LEADER text, MIRROR's Y/N default...).
            for (int i = 0; i < 3 && r.ok && r.pending; ++i)
                r = provideInput(InputValue::makeFinish());
            if (r.ok && r.pending) {
                cancelActive();
                return {false, QStringLiteral("incomplete input for command %1").arg(name),
                        false, {}};
            }
        }
        return r;
    }

    // A command is active: the line is input for it.
    if (tokens.isEmpty())
        return provideInput(InputValue::makeFinish());
    m_pendingTokens << tokens;
    return feedPendingTokens();
}

CommandProcessor::Result CommandProcessor::provideInput(const InputValue& value)
{
    if (!m_active)
        return {false, QStringLiteral("no active command"), false, {}};
    return drive(m_active->onInput(m_ctx, value));
}

CommandProcessor::Result CommandProcessor::drive(Step step)
{
    switch (step.state) {
    case Step::State::Done:
        finishCommand(false);
        return {};
    case Step::State::Cancelled:
        finishCommand(true);
        return {true, {}, false, {}};
    case Step::State::Continue:
        m_currentRequest = step.request;
        return {true, {}, true, step.request.prompt};
    }
    return {};
}

CommandProcessor::Result CommandProcessor::feedPendingTokens()
{
    Result r{true, {}, true, m_currentRequest.prompt};
    while (m_active && !m_pendingTokens.isEmpty()) {
        QString error;
        // Text consumes the whole remaining line.
        if (m_currentRequest.kind == InputKind::Text) {
            const QString joined = m_pendingTokens.join(QLatin1Char(' '));
            m_pendingTokens.clear();
            r = provideInput(InputValue::makeText(joined));
            continue;
        }
        // EntitySet consumes every remaining numeric token in one gulp.
        if (m_currentRequest.kind == InputKind::EntitySet) {
            std::vector<EntityId> ids;
            while (!m_pendingTokens.isEmpty()) {
                bool ok = false;
                const qlonglong id = m_pendingTokens.first().toLongLong(&ok);
                if (!ok)
                    break;
                ids.push_back(id);
                m_pendingTokens.removeFirst();
            }
            if (ids.empty())
                return {false, QStringLiteral("expected entity ids"), false, {}};
            r = provideInput(InputValue::makeEntitySet(std::move(ids)));
            continue;
        }
        const QString token = m_pendingTokens.takeFirst();
        const auto value = parseToken(token, error);
        if (!value) {
            cancelActive();
            return {false, error, false, {}};
        }
        r = provideInput(*value);
        if (!r.ok || !r.pending)
            break;
    }
    return r;
}

std::optional<InputValue> CommandProcessor::parseToken(const QString& token, QString& error)
{
    switch (m_currentRequest.kind) {
    case InputKind::Point: {
        const auto p = parsePointToken(token, m_ctx.lastPoint(), m_ctx.unitFactor());
        if (p)
            return InputValue::makePoint(*p);
        // Alphabetic tokens at a point prompt are command keywords (PLINE C).
        bool alpha = !token.isEmpty();
        for (const QChar c : token)
            alpha = alpha && c.isLetter();
        if (alpha)
            return InputValue::makeKeyword(token.toUpper());
        error = QStringLiteral("invalid point: %1").arg(token);
        return std::nullopt;
    }
    case InputKind::Distance: {
        if (const auto n = parseLengthToken(token, m_ctx.unitFactor()))
            return InputValue::makeNumber(*n);
        const auto p = parsePointToken(token, m_ctx.lastPoint(), m_ctx.unitFactor());
        if (p)
            return InputValue::makeNumber(p->distanceTo(m_ctx.lastPoint()));
        error = QStringLiteral("invalid distance: %1").arg(token);
        return std::nullopt;
    }
    case InputKind::Number: {
        bool ok = false;
        const double n = token.toDouble(&ok);
        if (ok)
            return InputValue::makeNumber(n);
        // A point is acceptable where a number is asked (angle/factor by point).
        if (const auto p = parsePointToken(token, m_ctx.lastPoint(), m_ctx.unitFactor()))
            return InputValue::makePoint(*p);
        error = QStringLiteral("invalid number: %1").arg(token);
        return std::nullopt;
    }
    case InputKind::Keyword:
        return InputValue::makeKeyword(token.toUpper());
    case InputKind::Text:
        return InputValue::makeText(token);
    case InputKind::EntitySet:
        // Handled wholesale in feedPendingTokens.
        error = QStringLiteral("internal: EntitySet token");
        return std::nullopt;
    }
    error = QStringLiteral("unhandled input kind");
    return std::nullopt;
}

void CommandProcessor::cancelActive()
{
    if (!m_active)
        return;
    finishCommand(true);
}

void CommandProcessor::finishCommand(bool cancelled)
{
    // Safety net: a command must not leak an open transaction.
    if (m_ctx.doc().inTransaction()) {
        if (cancelled)
            m_ctx.doc().rollbackTransaction();
        else
            m_ctx.doc().commitTransaction();
    }
    m_active.reset();
    m_pendingTokens.clear();
    m_currentRequest = {};
}

} // namespace viki
