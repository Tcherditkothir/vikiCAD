#include <cstdio>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include "Version.h"
#include "cmd/CommandProcessor.h"
#include "doc/SelectionSet.h"
#include "io/NativeStore.h"
#include "script/ScriptRunner.h"
#include "solid/OcctOps.h"

// Headless CLI for VikiCAD. Every output is a single JSON object on stdout:
//   {"ok":true,"result":{...}}  or  {"ok":false,"error":{"code","message"}}
// so agents can pipe it straight into a JSON parser.

using namespace viki;

namespace {

int emitJson(const QJsonObject& obj)
{
    const QJsonObject root = obj;
    std::printf("%s\n", QJsonDocument(root).toJson(QJsonDocument::Compact).constData());
    return root[QStringLiteral("ok")].toBool() ? 0 : 1;
}

int emitOk(QJsonObject result)
{
    return emitJson(QJsonObject{{QStringLiteral("ok"), true},
                            {QStringLiteral("result"), result}});
}

int emitError(const QString& code, const QString& message)
{
    return emitJson(QJsonObject{
        {QStringLiteral("ok"), false},
        {QStringLiteral("error"),
         QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}});
}

int printUsage(FILE* out)
{
    std::fprintf(out,
        "usage:\n"
        "  vikicad-cli --version\n"
        "  vikicad-cli new  [--exec \"CMD ...\"]... [--run script.vks] --save-as OUT.vkd\n"
        "  vikicad-cli open FILE.vkd [--exec \"CMD ...\"]... [--run script.vks]\n"
        "              [--save] [--save-as OUT.vkd]\n"
        "  vikicad-cli query FILE.vkd [--entities] [--layers] [--bounds]\n"
        "All output is JSON on stdout.\n");
    return out == stdout ? 0 : 2;
}

QJsonObject entityToJsonWithId(const Document& doc, EntityId id)
{
    const Entity* e = doc.entity(id);
    QJsonObject obj = e->toJson();
    obj[QStringLiteral("id")] = qint64(id);
    const BBox2d b = e->bounds();
    obj[QStringLiteral("bounds")] = QJsonArray{b.min.x, b.min.y, b.max.x, b.max.y};
    return obj;
}

int cmdQuery(const QStringList& args)
{
    if (args.isEmpty())
        return emitError(QStringLiteral("E_ARGS"), QStringLiteral("query needs a file"));
    const QString path = args.first();
    QString error;
    const auto doc = NativeStore::load(path, error);
    if (!doc)
        return emitError(QStringLiteral("E_OPEN"), error);

    const bool wantEntities = args.contains(QLatin1String("--entities"));
    const bool wantLayers = args.contains(QLatin1String("--layers"));
    const bool wantBounds = args.contains(QLatin1String("--bounds"));

    QJsonObject result;
    result[QStringLiteral("file")] = path;
    result[QStringLiteral("count")] = qint64(doc->entityCount());

    if (wantEntities || (!wantLayers && !wantBounds)) {
        QJsonArray entities;
        for (const EntityId id : doc->drawOrder())
            entities.append(entityToJsonWithId(*doc, id));
        result[QStringLiteral("entities")] = entities;
    }
    if (wantLayers) {
        QJsonArray layers;
        for (const Layer& l : doc->layers())
            layers.append(QJsonObject{
                {QStringLiteral("id"), qint64(l.id)},
                {QStringLiteral("name"), l.name},
                {QStringLiteral("color"),
                 QStringLiteral("#%1").arg(l.rgb, 6, 16, QLatin1Char('0'))},
                {QStringLiteral("visible"), l.visible},
                {QStringLiteral("locked"), l.locked}});
        result[QStringLiteral("layers")] = layers;
    }
    if (wantBounds) {
        const BBox2d b = doc->extents();
        result[QStringLiteral("bounds")] =
            b.isValid() ? QJsonArray{b.min.x, b.min.y, b.max.x, b.max.y} : QJsonArray{};
    }
    return emitOk(result);
}

int cmdNewOrOpen(const QString& verb, QStringList args)
{
    std::unique_ptr<Document> doc;
    QString openedPath;

    if (verb == QLatin1String("open")) {
        if (args.isEmpty() || args.first().startsWith(QLatin1String("--")))
            return emitError(QStringLiteral("E_ARGS"), QStringLiteral("open needs a file"));
        openedPath = args.takeFirst();
        QString error;
        doc = NativeStore::load(openedPath, error);
        if (!doc)
            return emitError(QStringLiteral("E_OPEN"), error);
    } else {
        doc = std::make_unique<Document>();
    }

    SelectionSet selection;
    CommandContext ctx(*doc, selection);
    CommandProcessor processor(ctx);
    registerBuiltinCommands(processor);

    int executed = 0;
    QString saveAs;
    bool save = false;

    for (int i = 0; i < args.size(); ++i) {
        const QString& a = args[i];
        if (a == QLatin1String("--exec")) {
            if (++i >= args.size())
                return emitError(QStringLiteral("E_ARGS"), QStringLiteral("--exec needs a value"));
            const auto r = processor.submit(args[i], /*strict=*/true);
            if (!r.ok)
                return emitError(QStringLiteral("E_EXEC"),
                                 QStringLiteral("%1 (in: %2)").arg(r.error, args[i]));
            ++executed;
        } else if (a == QLatin1String("--run")) {
            if (++i >= args.size())
                return emitError(QStringLiteral("E_ARGS"), QStringLiteral("--run needs a file"));
            const auto r = runScriptFile(processor, args[i]);
            if (!r.ok)
                return emitError(QStringLiteral("E_SCRIPT"),
                                 QStringLiteral("line %1: %2").arg(r.lineNumber).arg(r.error));
            ++executed;
        } else if (a == QLatin1String("--save")) {
            save = true;
        } else if (a == QLatin1String("--save-as")) {
            if (++i >= args.size())
                return emitError(QStringLiteral("E_ARGS"), QStringLiteral("--save-as needs a path"));
            saveAs = args[i];
        } else {
            return emitError(QStringLiteral("E_ARGS"), QStringLiteral("unknown option: %1").arg(a));
        }
    }

    QString savedTo;
    if (!saveAs.isEmpty())
        savedTo = saveAs;
    else if (save && !openedPath.isEmpty())
        savedTo = openedPath;
    if (save && openedPath.isEmpty() && saveAs.isEmpty())
        return emitError(QStringLiteral("E_ARGS"),
                         QStringLiteral("new document: use --save-as, not --save"));
    if (!savedTo.isEmpty()) {
        QString error;
        if (!NativeStore::save(*doc, savedTo, error))
            return emitError(QStringLiteral("E_SAVE"), error);
    }

    QJsonObject result;
    result[QStringLiteral("executed")] = executed;
    result[QStringLiteral("entityCount")] = qint64(doc->entityCount());
    if (!savedTo.isEmpty())
        result[QStringLiteral("savedTo")] = savedTo;
    QJsonArray messages;
    for (const QString& m : ctx.messages())
        messages.append(m);
    result[QStringLiteral("messages")] = messages;
    return emitOk(result);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = QCoreApplication::arguments();
    args.removeFirst();

    if (args.isEmpty())
        return printUsage(stderr);

    const QString verb = args.takeFirst();
    if (verb == QLatin1String("--version"))
        return emitOk(QJsonObject{{QStringLiteral("app"), QStringLiteral("vikicad-cli")},
                                  {QStringLiteral("version"), QLatin1String(versionString())},
                                  {QStringLiteral("occt"), QLatin1String(occtVersionString())},
                                  {QStringLiteral("occtSmoke"), occtSmokeTest()}});
    if (verb == QLatin1String("--help"))
        return printUsage(stdout);
    if (verb == QLatin1String("query"))
        return cmdQuery(args);
    if (verb == QLatin1String("new") || verb == QLatin1String("open"))
        return cmdNewOrOpen(verb, args);

    return emitError(QStringLiteral("E_UNKNOWN_VERB"),
                     QStringLiteral("unknown verb: %1").arg(verb));
}
