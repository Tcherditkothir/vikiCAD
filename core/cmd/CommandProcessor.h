#pragma once

#include <map>
#include <memory>

#include <QStringList>

#include "Command.h"
#include "CommandContext.h"

namespace viki {

// The single command brain. The GUI command bar, mouse tools, script runner,
// headless CLI and IPC server all drive commands exclusively through this.
class CommandProcessor {
public:
    explicit CommandProcessor(CommandContext& ctx);

    void registerCommand(CommandFactory factory, const QStringList& aliases = {});

    struct Result {
        bool ok = true;
        QString error;
        // True when a command is still waiting for interactive input.
        bool pending = false;
        QString prompt;
    };

    // Submit a text line. If a command is active, the line is fed to it as
    // input values; otherwise the first token is the command name.
    // strict = headless: a command left incomplete after all tokens (and one
    // implicit Finish) is an error instead of staying pending.
    Result submit(const QString& line, bool strict = false);

    // Feed one structured input (mouse click, Enter, Escape) to the active command.
    Result provideInput(const InputValue& value);

    bool hasActiveCommand() const { return m_active != nullptr; }
    Command* activeCommand() { return m_active.get(); }
    const InputRequest& currentRequest() const { return m_currentRequest; }
    void cancelActive();

    CommandContext& ctx() { return m_ctx; }

private:
    Result drive(Step step);
    Result feedPendingTokens();
    // Parse one token for the currently requested input kind.
    std::optional<InputValue> parseToken(const QString& token, QString& error);
    void finishCommand(bool cancelled);

    CommandContext& m_ctx;
    std::map<QString, CommandFactory> m_registry; // key: uppercase name/alias
    std::unique_ptr<Command> m_active;
    InputRequest m_currentRequest;
    QStringList m_pendingTokens;
};

// Registers every built-in command (drawing, editing, view).
void registerBuiltinCommands(CommandProcessor& processor);
// Individual groups (called by registerBuiltinCommands).
void registerDrawCommands2(CommandProcessor& processor);
void registerModifyCommands(CommandProcessor& processor);
void registerEditCommands(CommandProcessor& processor);
void registerAnnotateCommands(CommandProcessor& processor);
void registerBlockCommands(CommandProcessor& processor);
void registerArrayNoteCommands(CommandProcessor& processor);
void registerLayoutCommands(CommandProcessor& processor);
void registerSolidCommands(CommandProcessor& processor);

} // namespace viki
