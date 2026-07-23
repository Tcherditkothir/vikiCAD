#pragma once

#include <optional>
#include <vector>

#include <QString>

#include "doc/Entity.h"

namespace viki {

// What a command asks for next.
enum class InputKind {
    Point,     // a 2D point
    Distance,  // a length in display units, or a point (distance from base)
    Number,    // a raw number (angle, factor, count) — NO unit conversion
    EntitySet, // one or more entities (ends with Finish in GUI mode)
    Text,      // free text
    Keyword,   // one of the command's keywords ("E" for ZOOM Extents, ...)
};

struct InputRequest {
    InputKind kind = InputKind::Point;
    QString prompt;
};

// One unit of user input, from any front end (mouse, command bar, CLI args).
struct InputValue {
    enum class Kind { Point, Number, EntityRef, EntitySet, Text, Keyword, Finish, Cancel };
    Kind kind;

    Vec2d point;
    double number = 0.0;
    EntityId entityRef = kInvalidEntityId;
    std::vector<EntityId> entitySet;
    QString text; // Text and Keyword

    static InputValue makePoint(const Vec2d& p) { return {Kind::Point, p, 0, 0, {}, {}}; }
    static InputValue makeNumber(double n) { return {Kind::Number, {}, n, 0, {}, {}}; }
    static InputValue makeEntityRef(EntityId id) { return {Kind::EntityRef, {}, 0, id, {}, {}}; }
    static InputValue makeEntitySet(std::vector<EntityId> ids)
    {
        return {Kind::EntitySet, {}, 0, 0, std::move(ids), {}};
    }
    static InputValue makeText(const QString& t) { return {Kind::Text, {}, 0, 0, {}, t}; }
    static InputValue makeKeyword(const QString& k) { return {Kind::Keyword, {}, 0, 0, {}, k}; }
    static InputValue makeFinish() { return {Kind::Finish, {}, 0, 0, {}, {}}; }
    static InputValue makeCancel() { return {Kind::Cancel, {}, 0, 0, {}, {}}; }
};

// Command step result.
struct Step {
    enum class State { Continue, Done, Cancelled };
    State state = State::Done;
    InputRequest request; // valid when Continue
    // Done WITHOUT consuming the input that triggered it: an optional stage
    // ended on a token that is not one of its keywords. The processor hands
    // the raw token (and the rest of the line) back as a NEW command line —
    // AutoCAD .scr semantics. Without this, 'WORKPLANE XZ' followed by
    // 'RECT ...' silently swallowed the whole RECT line (empty document,
    // ok:true — the .vks trap).
    bool repush = false;

    static Step cont(InputKind kind, const QString& prompt)
    {
        Step s;
        s.state = State::Continue;
        s.request = {kind, prompt};
        return s;
    }
    static Step done() { return {}; }
    static Step doneRepush()
    {
        Step s;
        s.repush = true;
        return s;
    }
    static Step cancelled()
    {
        Step s;
        s.state = State::Cancelled;
        return s;
    }
};

// Parse a length token in display units. Suffixes override the mode:
// 2" or 2in = inches, 2mm = millimeters, bare 2 = unitFactor (mm per unit).
std::optional<double> parseLengthToken(const QString& token, double unitFactor);

// Parse a coordinate token: "x,y" absolute, "@dx,dy" relative,
// "@dist<angle_deg" polar relative. Coordinates honor unit suffixes and
// unitFactor. Returns nullopt on syntax error.
std::optional<Vec2d> parsePointToken(const QString& token, const Vec2d& lastPoint,
                                     double unitFactor = 1.0);

} // namespace viki
