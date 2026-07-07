#pragma once

#include <memory>

#include "Input.h"

namespace viki {

class CommandContext;

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
};

using CommandFactory = std::unique_ptr<Command> (*)();

} // namespace viki
