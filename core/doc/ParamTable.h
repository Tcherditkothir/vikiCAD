#pragma once

#include <vector>

#include <QString>

namespace viki {

// A named user parameter: an expression string plus its last evaluated value.
// Expressions may reference other parameters by name (w = 2*d).
struct Param {
    QString name;
    QString expr;      // source expression, e.g. "2*d"
    double value = 0.0; // cached result of the last evaluate()
    bool valid = false; // false if the expression failed (unknown ref, cycle, syntax)
};

// A table of user parameters with a small expression evaluator. This is
// groundwork for driving dimensions/features later: named values that can be
// expressed in terms of each other (d = 10, w = 2*d).
//
// The evaluator supports: decimal numbers, + - * / (with precedence),
// unary +/-, parentheses, and references to other parameters by name.
// Every set() re-evaluates the whole table so dependents track their inputs;
// cycles and unknown references are detected and mark the offending param
// invalid rather than throwing.
class ParamTable {
public:
    // Insert or replace `name` with `expr`, then re-evaluate the whole table.
    // Returns false (and leaves the param marked invalid) if the assignment
    // makes `name` unresolvable — but the param is still stored so the user can
    // fix it. Names are case-sensitive and must be a valid identifier.
    bool set(const QString& name, const QString& expr);

    // Remove a parameter. Returns false if it did not exist. Re-evaluates the
    // rest (dependents on the removed name become invalid).
    bool remove(const QString& name);

    void clear() { m_params.clear(); }

    bool contains(const QString& name) const;
    const Param* find(const QString& name) const;

    // Evaluated value of `name`; `ok` reports whether it resolved.
    double value(const QString& name, bool* ok = nullptr) const;

    const std::vector<Param>& params() const { return m_params; }
    size_t size() const { return m_params.size(); }
    bool empty() const { return m_params.empty(); }

    // Re-evaluate every parameter (fixed point over dependencies). Called
    // automatically by set()/remove(); exposed for the load path.
    void reevaluate();

    // Restore a raw param (load path). Does not evaluate; call reevaluate() after.
    void restore(const QString& name, const QString& expr)
    {
        upsert(name, expr);
    }

    // A legal identifier: starts with a letter or '_', then letters/digits/'_'.
    static bool isValidName(const QString& name);

private:
    Param* upsert(const QString& name, const QString& expr);
    std::vector<Param> m_params;
};

} // namespace viki
