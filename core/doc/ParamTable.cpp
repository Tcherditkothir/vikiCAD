#include "ParamTable.h"

#include <cmath>
#include <functional>

#include <QHash>
#include <QSet>

namespace viki {
namespace {

// A tiny recursive-descent evaluator for arithmetic over parameter references.
// Grammar:
//   expr   := term (('+' | '-') term)*
//   term   := factor (('*' | '/') factor)*
//   factor := ('+' | '-') factor | number | ident | '(' expr ')'
// References are resolved through a callback that already guards against
// cycles (see ParamTable::reevaluate), so the evaluator itself is pure.
class Evaluator {
public:
    using Resolver = std::function<bool(const QString&, double&)>;

    Evaluator(const QString& src, Resolver resolver)
        : m_src(src), m_resolve(std::move(resolver))
    {
    }

    bool run(double& out)
    {
        m_pos = 0;
        m_ok = true;
        skipSpace();
        const double v = parseExpr();
        skipSpace();
        if (!m_ok || m_pos != m_src.size())
            return false;
        out = v;
        return true;
    }

private:
    void skipSpace()
    {
        while (m_pos < m_src.size() && m_src.at(m_pos).isSpace())
            ++m_pos;
    }

    QChar peek() const { return m_pos < m_src.size() ? m_src.at(m_pos) : QChar(); }

    double parseExpr()
    {
        double v = parseTerm();
        for (;;) {
            skipSpace();
            const QChar c = peek();
            if (c == QLatin1Char('+')) {
                ++m_pos;
                v += parseTerm();
            } else if (c == QLatin1Char('-')) {
                ++m_pos;
                v -= parseTerm();
            } else {
                break;
            }
        }
        return v;
    }

    double parseTerm()
    {
        double v = parseFactor();
        for (;;) {
            skipSpace();
            const QChar c = peek();
            if (c == QLatin1Char('*')) {
                ++m_pos;
                v *= parseFactor();
            } else if (c == QLatin1Char('/')) {
                ++m_pos;
                const double d = parseFactor();
                if (d == 0.0) {
                    m_ok = false;
                    return 0.0;
                }
                v /= d;
            } else {
                break;
            }
        }
        return v;
    }

    double parseFactor()
    {
        skipSpace();
        const QChar c = peek();
        if (c == QLatin1Char('+')) {
            ++m_pos;
            return parseFactor();
        }
        if (c == QLatin1Char('-')) {
            ++m_pos;
            return -parseFactor();
        }
        if (c == QLatin1Char('(')) {
            ++m_pos;
            const double v = parseExpr();
            skipSpace();
            if (peek() != QLatin1Char(')')) {
                m_ok = false;
                return 0.0;
            }
            ++m_pos;
            return v;
        }
        if (c.isDigit() || c == QLatin1Char('.'))
            return parseNumber();
        if (c.isLetter() || c == QLatin1Char('_'))
            return parseIdent();
        m_ok = false;
        return 0.0;
    }

    double parseNumber()
    {
        const int start = m_pos;
        while (m_pos < m_src.size() &&
               (m_src.at(m_pos).isDigit() || m_src.at(m_pos) == QLatin1Char('.')))
            ++m_pos;
        // Optional exponent (1e3, 2.5E-2).
        if (m_pos < m_src.size() &&
            (m_src.at(m_pos) == QLatin1Char('e') || m_src.at(m_pos) == QLatin1Char('E'))) {
            int p = m_pos + 1;
            if (p < m_src.size() &&
                (m_src.at(p) == QLatin1Char('+') || m_src.at(p) == QLatin1Char('-')))
                ++p;
            if (p < m_src.size() && m_src.at(p).isDigit()) {
                m_pos = p;
                while (m_pos < m_src.size() && m_src.at(m_pos).isDigit())
                    ++m_pos;
            }
        }
        bool ok = false;
        const double v = m_src.mid(start, m_pos - start).toDouble(&ok);
        if (!ok)
            m_ok = false;
        return v;
    }

    double parseIdent()
    {
        const int start = m_pos;
        while (m_pos < m_src.size() &&
               (m_src.at(m_pos).isLetterOrNumber() || m_src.at(m_pos) == QLatin1Char('_')))
            ++m_pos;
        const QString name = m_src.mid(start, m_pos - start);
        double v = 0.0;
        if (!m_resolve(name, v)) {
            m_ok = false;
            return 0.0;
        }
        return v;
    }

    const QString& m_src;
    Resolver m_resolve;
    int m_pos = 0;
    bool m_ok = true;
};

} // namespace

bool ParamTable::isValidName(const QString& name)
{
    if (name.isEmpty())
        return false;
    const QChar first = name.at(0);
    if (!first.isLetter() && first != QLatin1Char('_'))
        return false;
    for (const QChar c : name)
        if (!c.isLetterOrNumber() && c != QLatin1Char('_'))
            return false;
    return true;
}

Param* ParamTable::upsert(const QString& name, const QString& expr)
{
    for (Param& p : m_params) {
        if (p.name == name) {
            p.expr = expr;
            return &p;
        }
    }
    m_params.push_back(Param{name, expr, 0.0, false});
    return &m_params.back();
}

bool ParamTable::set(const QString& name, const QString& expr)
{
    if (!isValidName(name))
        return false;
    upsert(name, expr);
    reevaluate();
    const Param* p = find(name);
    return p && p->valid;
}

bool ParamTable::remove(const QString& name)
{
    for (auto it = m_params.begin(); it != m_params.end(); ++it) {
        if (it->name == name) {
            m_params.erase(it);
            reevaluate();
            return true;
        }
    }
    return false;
}

bool ParamTable::contains(const QString& name) const
{
    return find(name) != nullptr;
}

const Param* ParamTable::find(const QString& name) const
{
    for (const Param& p : m_params)
        if (p.name == name)
            return &p;
    return nullptr;
}

double ParamTable::value(const QString& name, bool* ok) const
{
    const Param* p = find(name);
    if (!p || !p->valid) {
        if (ok)
            *ok = false;
        return 0.0;
    }
    if (ok)
        *ok = true;
    return p->value;
}

void ParamTable::reevaluate()
{
    // Index by name for reference resolution.
    QHash<QString, Param*> byName;
    for (Param& p : m_params) {
        p.valid = false;
        byName.insert(p.name, &p);
    }

    // Memoized recursive evaluation with an active-set for cycle detection.
    QHash<QString, double> cache;
    QSet<QString> active;

    std::function<bool(const QString&, double&)> evalName =
        [&](const QString& name, double& out) -> bool {
        const auto cached = cache.constFind(name);
        if (cached != cache.constEnd()) {
            out = cached.value();
            return true;
        }
        if (active.contains(name)) // cycle
            return false;
        const auto it = byName.constFind(name);
        if (it == byName.constEnd()) // unknown reference
            return false;
        active.insert(name);
        Evaluator ev(it.value()->expr,
                     [&](const QString& ref, double& v) { return evalName(ref, v); });
        double v = 0.0;
        const bool ok = ev.run(v);
        active.remove(name);
        if (!ok)
            return false;
        cache.insert(name, v);
        out = v;
        return true;
    };

    for (Param& p : m_params) {
        double v = 0.0;
        if (evalName(p.name, v)) {
            p.value = v;
            p.valid = true;
        } else {
            p.value = 0.0;
            p.valid = false;
        }
    }
}

} // namespace viki
