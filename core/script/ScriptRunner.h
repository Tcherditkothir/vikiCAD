#pragma once

#include <QString>

#include "cmd/CommandProcessor.h"

namespace viki {

// Executes a .vks script: one command line per line, '#' comments, blank
// lines feed a Finish to a pending command (like pressing Enter).
// Runs in strict mode; stops at the first error.
struct ScriptResult {
    bool ok = true;
    int lineNumber = 0; // line of the failure when !ok
    QString error;
};

ScriptResult runScript(CommandProcessor& processor, const QString& scriptText);
ScriptResult runScriptFile(CommandProcessor& processor, const QString& path);

} // namespace viki
