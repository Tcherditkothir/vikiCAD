#pragma once

#include <memory>

#include <TopoDS_Shape.hxx>

#include "Input.h"

namespace viki {

class CommandContext;

// Transient 3D ghost of a command's pending result, drawn by the 3D view while
// the mouse moves. The effect drives the colour (Fusion convention): Remove =
// red translucent (material will be cut away), Add = blue translucent
// (material will be added), Neutral = grey.
struct Preview3d {
    TopoDS_Shape shape;
    enum class Effect { Neutral, Add, Remove } effect = Effect::Neutral;
};

// An interactive command as an explicit state machine. start() returns the
// first request (or Done for immediate commands); each onInput() consumes one
// value and returns the next step. The command opens/commits its own document
// transaction through the context.
class Command {
public:
    virtual ~Command() = default;
    virtual const char* name() const = 0;
    virtual Step start(CommandContext& ctx) = 0;
    virtual Step onInput(CommandContext& ctx, const InputValue& value) = 0;
    // Live preview points for the GUI overlay (rubber band). Optional.
    virtual void previewAt(CommandContext& ctx, const Vec2d& cursor, PrimitiveList& out)
    {
        (void)ctx; (void)cursor; (void)out;
    }
    // Live 3D ghost at `cursor` (work-plane coords). Return true and fill
    // `out` to show it; the 3D view colours it by out.effect. Optional.
    virtual bool preview3d(CommandContext& ctx, const Vec2d& cursor, Preview3d& out)
    {
        (void)ctx; (void)cursor; (void)out;
        return false;
    }
};

using CommandFactory = std::unique_ptr<Command> (*)();

} // namespace viki
