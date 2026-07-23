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

    // All registered command names and aliases, sorted — for autocompletion.
    QStringList commandNames() const;

    // Autocompletion rows: canonical names as-is, aliases as "REC → RECT"
    // (the arrow teaches what the alias runs). Sorted.
    QStringList completionEntries() const;

    // Resolve typed text to a registered command key: exact name/alias
    // first, then UNIQUE-prefix resolution over the whole registry (REC →
    // RECT). Aliases of the SAME command don't make a prefix ambiguous
    // (REC matches RECT and RECTANGLE — one command). Returns the canonical
    // (uppercase) name, or an empty string with `errorOut` filled when the
    // text is unknown or ambiguous (the candidates are listed so the user
    // can type more letters — nothing is ever run on an ambiguous prefix).
    QString resolveName(const QString& typed, QString* errorOut = nullptr) const;

    CommandContext& ctx() { return m_ctx; }

private:
    Result drive(Step step);
    Result feedPendingTokens();
    // Parse one token for the currently requested input kind.
    std::optional<InputValue> parseToken(const QString& token, QString& error);
    void finishCommand(bool cancelled);

    CommandContext& m_ctx;
    std::map<QString, CommandFactory> m_registry; // key: uppercase name/alias
    std::map<QString, QString> m_canonical;       // key -> canonical name
    std::unique_ptr<Command> m_active;
    InputRequest m_currentRequest;
    QStringList m_pendingTokens;
    // Set by drive() when a command finished with Step::repush: the token
    // that triggered the finish was NOT consumed and must start a new
    // command line (only meaningful under feedPendingTokens, where the raw
    // token text is known). Reset on every drive().
    bool m_repushPending = false;
    bool m_strict = false; // strictness of the submit() currently running
};

// Registers every built-in command (drawing, editing, view).
void registerBuiltinCommands(CommandProcessor& processor);
// Individual groups (called by registerBuiltinCommands).
void registerDrawCommands2(CommandProcessor& processor);
void registerLayerCommands(CommandProcessor& processor);
void registerModifyCommands(CommandProcessor& processor);
void registerEditCommands(CommandProcessor& processor);
void registerAnnotateCommands(CommandProcessor& processor);
void registerBlockCommands(CommandProcessor& processor);
void registerArrayNoteCommands(CommandProcessor& processor);
void registerLayoutCommands(CommandProcessor& processor);
void registerSolidCommands(CommandProcessor& processor);
void registerSolidFinishCommands(CommandProcessor& processor);
void registerMeasureCommands(CommandProcessor& processor);
void registerCamCommands(CommandProcessor& processor);
void registerGearCommands(CommandProcessor& processor);
void registerAssemblyCommands(CommandProcessor& processor);
void registerPattern3DCommands(CommandProcessor& processor);
void registerParamCommands(CommandProcessor& processor);
void registerSketchCommands(CommandProcessor& processor);
void registerInspectCommands(CommandProcessor& processor);
void registerDescribeCommands(CommandProcessor& processor);
void registerFeatEditCommands(CommandProcessor& processor);
void registerSubShapeOpCommands(CommandProcessor& processor);

} // namespace viki
