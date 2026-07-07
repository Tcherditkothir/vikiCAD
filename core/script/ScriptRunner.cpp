#include "ScriptRunner.h"

#include <QFile>
#include <QTextStream>

namespace viki {

ScriptResult runScript(CommandProcessor& processor, const QString& scriptText)
{
    const QStringList lines = scriptText.split(QLatin1Char('\n'));
    int lineNo = 0;
    for (const QString& raw : lines) {
        ++lineNo;
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('#')))
            continue;
        if (line.isEmpty()) {
            if (processor.hasActiveCommand())
                processor.provideInput(InputValue::makeFinish());
            continue;
        }
        // Non-strict: a command may keep prompting across script lines
        // (AutoCAD .scr behavior — values continue on following lines).
        const auto r = processor.submit(line, /*strict=*/false);
        if (!r.ok)
            return {false, lineNo, r.error};
    }
    // A command left pending at EOF gets a Finish, then must terminate.
    if (processor.hasActiveCommand()) {
        processor.provideInput(InputValue::makeFinish());
        if (processor.hasActiveCommand()) {
            processor.cancelActive();
            return {false, lineNo, QStringLiteral("script ended with incomplete command")};
        }
    }
    return {};
}

ScriptResult runScriptFile(CommandProcessor& processor, const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {false, 0, QStringLiteral("cannot open script: %1").arg(path)};
    QTextStream in(&file);
    return runScript(processor, in.readAll());
}

} // namespace viki
