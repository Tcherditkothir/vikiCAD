#include <cstdio>

#include <QFileInfo>
#include <QGuiApplication>
#include <QLocalSocket>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include "Version.h"
#include "cmd/CommandProcessor.h"
#include "doc/SelectionSet.h"
#ifdef VIKICAD_HAS_DXF
#include "io/DxfExporter.h"
#include "io/DxfImporter.h"
#endif
#include "io/ExcellonWriter.h"
#include "io/GerberKit.h"
#include "io/GerberKitWriter.h"
#include "io/NativeStore.h"
#include "io/PdfPlotter.h"
#include "io/StepIo.h"
#include "io/StlIo.h"
#include "io/ObjIo.h"
#include "io/QueryJson.h"
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
        "              [--notes] [--blocks] [--layouts] [--describe]\n"
        "  vikicad-cli import IN.dxf|IN.dwg --save-as OUT.vkd\n"
        "  vikicad-cli import KITDIR|IN.gbr|IN.txt --save-as OUT.vkd\n"
        "              (Gerber kit: directory or single Gerber/Excellon file)\n"
        "  vikicad-cli export FILE.vkd OUT.dxf [--dxf-version R12|...|2018]\n"
        "  vikicad-cli export FILE.vkd OUT.pdf [--layout NAME] [--with-notes]\n"
        "  vikicad-cli export FILE.vkd OUT.step   (solids + notes sidecar)\n"
        "  vikicad-cli export FILE.vkd OUT.stl [--deflection MM] [--ascii]\n"
        "  vikicad-cli export FILE.vkd OUTDIR   (Gerber kit: OUTDIR exists or ends "
        "with '/'; writes <base>.GTL/... + .TXT)\n"
        "  vikicad-cli export FILE.vkd OUT.gtl|.gbs|...|.gko|.gbr|.txt "
        "[--layer NAME]   (one fab layer)\n"
        "  vikicad-cli import IN.step --save-as OUT.vkd\n"
        "  vikicad-cli connect METHOD [ARGS...]   (talk to a running GUI)\n"
        "All output is JSON on stdout.\n");
    return out == stdout ? 0 : 2;
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
    const bool wantNotes = args.contains(QLatin1String("--notes"));
    const bool wantBlocks = args.contains(QLatin1String("--blocks"));
    const bool wantLayouts = args.contains(QLatin1String("--layouts"));
    const bool wantDescribe = args.contains(QLatin1String("--describe"));
    const bool anyFlag = wantEntities || wantLayers || wantBounds || wantNotes ||
                         wantBlocks || wantLayouts || wantDescribe;

    QJsonObject result;
    result[QStringLiteral("file")] = path;
    result[QStringLiteral("count")] = qint64(doc->entityCount());

    if (wantEntities || !anyFlag)
        result[QStringLiteral("entities")] = queryjson::entitiesJson(*doc);
    if (wantLayers)
        result[QStringLiteral("layers")] = queryjson::layersJson(*doc);
    if (wantBounds)
        result[QStringLiteral("bounds")] = queryjson::boundsJson(*doc);
    if (wantNotes)
        result[QStringLiteral("notes")] = queryjson::notesJson(*doc);
    if (wantBlocks)
        result[QStringLiteral("blocks")] = queryjson::blocksJson(*doc);
    if (wantLayouts)
        result[QStringLiteral("layouts")] = queryjson::layoutsJson(*doc);
    if (wantDescribe)
        result[QStringLiteral("describe")] = queryjson::describeJson(*doc);
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

int cmdImport(const QStringList& args)
{
    if (args.size() < 3 || args[1] != QLatin1String("--save-as"))
        return emitError(QStringLiteral("E_ARGS"),
                         QStringLiteral("usage: import IN.dxf|IN.step|KITDIR|"
                                        "IN.gbr --save-as OUT.vkd"));
    const QString inPath = args[0];
    const QString outPath = args[2];

    // Gerber kit: a directory, or any single file that is neither DXF/DWG
    // nor STEP (the kit importer sniffs Gerber/Excellon content and reports
    // a clear error for anything else).
    const bool dxfLike = inPath.endsWith(QLatin1String(".dxf"), Qt::CaseInsensitive) ||
                         inPath.endsWith(QLatin1String(".dwg"), Qt::CaseInsensitive);
    const bool stepLike = inPath.endsWith(QLatin1String(".step"), Qt::CaseInsensitive) ||
                          inPath.endsWith(QLatin1String(".stp"), Qt::CaseInsensitive);
    if (QFileInfo(inPath).isDir() || (!dxfLike && !stepLike)) {
        Document doc;
        const GerberKitResult r = importGerberKit(doc, inPath);
        if (!r.ok)
            return emitError(QStringLiteral("E_GERBERKIT"), r.error);
        QString error;
        if (!NativeStore::save(doc, outPath, error))
            return emitError(QStringLiteral("E_SAVE"), error);
        QJsonObject result;
        result[QStringLiteral("files")] = int(r.files.size());
        result[QStringLiteral("entities")] = r.entities;
        result[QStringLiteral("savedTo")] = outPath;
        QJsonArray layers;
        for (const QString& l : r.layers)
            layers.append(l);
        result[QStringLiteral("layers")] = layers;
        QJsonArray skipped;
        for (const QString& s : r.skipped)
            skipped.append(s);
        result[QStringLiteral("skipped")] = skipped;
        QJsonArray warnings;
        for (const QString& w : r.warnings)
            warnings.append(w);
        result[QStringLiteral("warnings")] = warnings;
        return emitOk(result);
    }

#ifndef VIKICAD_HAS_DXF
    if (dxfLike)
        return emitError(QStringLiteral("E_NODXF"),
                         QStringLiteral("built without DXF support"));
#endif

    if (inPath.endsWith(QLatin1String(".step"), Qt::CaseInsensitive) ||
        inPath.endsWith(QLatin1String(".stp"), Qt::CaseInsensitive)) {
        std::unique_ptr<Document> doc;
        const StepResult r = importStep(inPath, doc);
        if (!r.ok)
            return emitError(QStringLiteral("E_STEP"), r.error);
        QString error;
        if (!NativeStore::save(*doc, outPath, error))
            return emitError(QStringLiteral("E_SAVE"), error);
        return emitOk(QJsonObject{{QStringLiteral("solids"), r.solids},
                                  {QStringLiteral("sidecarNotes"), r.notes},
                                  {QStringLiteral("savedTo"), outPath}});
    }

#ifdef VIKICAD_HAS_DXF
    DxfImportResult r = inPath.endsWith(QLatin1String(".dwg"), Qt::CaseInsensitive)
                            ? importDwg(inPath)
                            : importDxf(inPath);
    if (!r.ok)
        return emitError(QStringLiteral("E_IMPORT"), r.error);
    QString error;
    if (!NativeStore::save(*r.document, outPath, error))
        return emitError(QStringLiteral("E_SAVE"), error);

    QJsonObject result;
    result[QStringLiteral("imported")] = r.imported;
    result[QStringLiteral("skipped")] = r.skipped;
    QJsonArray skippedTypes;
    for (const QString& t : r.skippedTypes)
        skippedTypes.append(t);
    result[QStringLiteral("skippedTypes")] = skippedTypes;
    result[QStringLiteral("savedTo")] = outPath;
    QJsonArray layers;
    for (const Layer& l : r.document->layers())
        layers.append(l.name);
    result[QStringLiteral("layers")] = layers;
    return emitOk(result);
#else
    return emitError(QStringLiteral("E_NODXF"),
                     QStringLiteral("built without DXF support"));
#endif
}

// Result JSON for the fab exports (kit directory or single layer).
QJsonObject fabFilesJson(const GerberKitExportResult& r)
{
    QJsonObject out;
    QJsonArray files;
    for (const GerberKitExportFile& f : r.files)
        files.append(QJsonObject{{QStringLiteral("path"), f.path},
                                 {QStringLiteral("layers"),
                                  QJsonArray::fromStringList(f.layers)},
                                 {QStringLiteral("drill"), f.isDrill},
                                 {QStringLiteral("entities"), f.entities},
                                 {QStringLiteral("skipped"), f.skipped}});
    out[QStringLiteral("files")] = files;
    out[QStringLiteral("skippedLayers")] = QJsonArray::fromStringList(r.skippedLayers);
    out[QStringLiteral("warnings")] = QJsonArray::fromStringList(r.warnings);
    return out;
}

// Gerber kit (OUT is a directory) or one fab layer (OUT has a fab extension).
// Returns -1 when OUT is neither — the caller falls through to DXF & co.
int cmdExportFab(const QString& inPath, const QString& outPath,
                 const QStringList& args)
{
    static const QStringList kFabExts{
        QStringLiteral("gtl"), QStringLiteral("gbl"), QStringLiteral("gts"),
        QStringLiteral("gbs"), QStringLiteral("gto"), QStringLiteral("gbo"),
        QStringLiteral("gtp"), QStringLiteral("gbp"), QStringLiteral("gko"),
        QStringLiteral("gbr"), QStringLiteral("ger"), QStringLiteral("txt"),
        QStringLiteral("drl")};
    const QString suffix = QFileInfo(outPath).suffix().toLower();
    const bool kitDir =
        QFileInfo(outPath).isDir() || outPath.endsWith(QLatin1Char('/'));
    if (!kitDir && !kFabExts.contains(suffix))
        return -1;

    QString error;
    const auto doc = NativeStore::load(inPath, error);
    if (!doc)
        return emitError(QStringLiteral("E_OPEN"), error);

    if (kitDir) {
        const GerberKitExportResult r = exportGerberKit(
            *doc, outPath, QFileInfo(inPath).completeBaseName());
        if (!r.ok)
            return emitError(QStringLiteral("E_GERBERKIT"), r.error);
        QJsonObject result = fabFilesJson(r);
        result[QStringLiteral("savedTo")] = outPath;
        return emitOk(result);
    }

    QString layerName;
    const int li = args.indexOf(QLatin1String("--layer"));
    if (li >= 0 && li + 1 < args.size())
        layerName = args[li + 1];
    if (layerName.isEmpty()) {
        const QStringList candidates = layersForKitExtension(*doc, suffix);
        if (candidates.isEmpty())
            return emitError(
                QStringLiteral("E_LAYER"),
                QStringLiteral("no layer matches .%1 — name one with --layer")
                    .arg(suffix));
        if (candidates.size() > 1 &&
            !(suffix == QLatin1String("txt") || suffix == QLatin1String("drl")))
            return emitError(QStringLiteral("E_LAYER"),
                             QStringLiteral("ambiguous .%1 (%2) — name one with "
                                            "--layer")
                                 .arg(suffix, candidates.join(QLatin1String(", "))));
        if (candidates.size() > 1) {
            // Drill file: several drill layers group into ONE Excellon file
            // (plated + NPTH sections), like the kit export.
            const ExcellonExportResult r = exportExcellon(*doc, candidates, outPath);
            if (!r.ok)
                return emitError(QStringLiteral("E_GERBEREXPORT"), r.error);
            return emitOk(QJsonObject{
                {QStringLiteral("savedTo"), outPath},
                {QStringLiteral("layers"), QJsonArray::fromStringList(candidates)},
                {QStringLiteral("drill"), true},
                {QStringLiteral("holes"), r.holes},
                {QStringLiteral("tools"), r.tools},
                {QStringLiteral("skipped"), r.skipped},
                {QStringLiteral("warnings"),
                 QJsonArray::fromStringList(r.warnings)}});
        }
        layerName = candidates.first();
    }
    const GerberKitExportResult r = exportFabLayer(*doc, layerName, outPath);
    if (!r.ok)
        return emitError(QStringLiteral("E_GERBEREXPORT"), r.error);
    QJsonObject result = fabFilesJson(r);
    result[QStringLiteral("savedTo")] = outPath;
    return emitOk(result);
}

int cmdExport(const QStringList& args)
{
    if (args.size() >= 2) {
        const int fab = cmdExportFab(args[0], args[1], args);
        if (fab >= 0)
            return fab;
    }
#ifndef VIKICAD_HAS_DXF
    (void)args;
    return emitError(QStringLiteral("E_NODXF"), QStringLiteral("built without DXF support"));
#else
    if (args.size() < 2)
        return emitError(QStringLiteral("E_ARGS"),
                         QStringLiteral("usage: export FILE.vkd OUT.dxf [--dxf-version V]"));
    const QString inPath = args[0];
    const QString outPath = args[1];
    QString version = QStringLiteral("2013");
    const int vi = args.indexOf(QLatin1String("--dxf-version"));
    if (vi >= 0 && vi + 1 < args.size())
        version = args[vi + 1];

    QString error;
    const auto doc = NativeStore::load(inPath, error);
    if (!doc)
        return emitError(QStringLiteral("E_OPEN"), error);

    if (outPath.endsWith(QLatin1String(".step"), Qt::CaseInsensitive) ||
        outPath.endsWith(QLatin1String(".stp"), Qt::CaseInsensitive)) {
        const StepResult r = exportStep(*doc, outPath);
        if (!r.ok)
            return emitError(QStringLiteral("E_STEP"), r.error);
        return emitOk(QJsonObject{{QStringLiteral("savedTo"), outPath},
                                  {QStringLiteral("solids"), r.solids},
                                  {QStringLiteral("sidecarNotes"), r.notes}});
    }

    if (outPath.endsWith(QLatin1String(".stl"), Qt::CaseInsensitive)) {
        double deflection = 0.1;
        const int di = args.indexOf(QLatin1String("--deflection"));
        if (di >= 0 && di + 1 < args.size())
            deflection = args[di + 1].toDouble();
        const bool ascii = args.contains(QLatin1String("--ascii"));
        const StlResult r = exportStl(*doc, outPath, deflection, ascii);
        if (!r.ok)
            return emitError(QStringLiteral("E_STL"), r.error);
        return emitOk(QJsonObject{{QStringLiteral("savedTo"), outPath},
                                  {QStringLiteral("solids"), r.solids},
                                  {QStringLiteral("format"),
                                   ascii ? QStringLiteral("ascii")
                                         : QStringLiteral("binary")}});
    }

    if (outPath.endsWith(QLatin1String(".obj"), Qt::CaseInsensitive)) {
        double deflection = 0.1;
        const int di = args.indexOf(QLatin1String("--deflection"));
        if (di >= 0 && di + 1 < args.size())
            deflection = args[di + 1].toDouble();
        const ObjResult r = exportObj(*doc, outPath, deflection);
        if (!r.ok)
            return emitError(QStringLiteral("E_OBJ"), r.error);
        return emitOk(QJsonObject{{QStringLiteral("savedTo"), outPath},
                                  {QStringLiteral("solids"), r.solids},
                                  {QStringLiteral("vertices"), r.vertices},
                                  {QStringLiteral("faces"), r.faces}});
    }

    if (outPath.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) {
        QString layoutName;
        const int li = args.indexOf(QLatin1String("--layout"));
        if (li >= 0 && li + 1 < args.size())
            layoutName = args[li + 1];
        Layout* layout = nullptr;
        if (!layoutName.isEmpty()) {
            layout = doc->layoutByName(layoutName);
            if (!layout)
                return emitError(QStringLiteral("E_LAYOUT"),
                                 QStringLiteral("no layout named %1").arg(layoutName));
        } else if (!doc->layouts().empty()) {
            layout = const_cast<Layout*>(&doc->layouts().front());
        }
        Layout autoLayout;
        if (!layout) {
            // No layout defined: fit everything on A4 landscape.
            autoLayout.name = QStringLiteral("AUTO");
            Viewport vp;
            vp.x = vp.y = 10;
            vp.w = 277;
            vp.h = 190;
            const BBox2d ext = doc->extents();
            if (ext.isValid()) {
                vp.center = ext.center();
                vp.scale = 0.95 * std::min(vp.w / std::max(ext.width(), 1e-6),
                                           vp.h / std::max(ext.height(), 1e-6));
            }
            autoLayout.viewports.push_back(vp);
            layout = &autoLayout;
        }
        if (!plotToPdf(*doc, *layout, outPath, error,
                       args.contains(QLatin1String("--with-notes"))))
            return emitError(QStringLiteral("E_PLOT"), error);
        return emitOk(QJsonObject{{QStringLiteral("savedTo"), outPath},
                                  {QStringLiteral("layout"), layout->name},
                                  {QStringLiteral("paper"),
                                   QJsonArray{layout->paperW, layout->paperH}}});
    }

    const DxfExportResult r = exportDxf(*doc, outPath, version);
    if (!r.ok)
        return emitError(QStringLiteral("E_EXPORT"), r.error);

    QJsonObject result;
    result[QStringLiteral("exported")] = r.exported;
    result[QStringLiteral("skipped")] = r.skipped;
    QJsonArray st;
    for (const QString& t : r.skippedTypes)
        st.append(t);
    result[QStringLiteral("skippedTypes")] = st;
    result[QStringLiteral("savedTo")] = outPath;
    result[QStringLiteral("dxfVersion")] = version;
    return emitOk(result);
#endif
}

int cmdConnect(const QStringList& args)
{
    if (args.isEmpty())
        return emitError(QStringLiteral("E_ARGS"),
                         QStringLiteral("connect needs a method: ping|exec|query|open|"
                                        "save|screenshot|view3d|viewdir|pick3d|"
                                        "export|insertstep|sketchface"));
    const QString method = args.first();
    QJsonObject params;
    if (method == QLatin1String("exec") && args.size() > 1)
        params[QStringLiteral("line")] = QStringList(args.mid(1)).join(QLatin1Char(' '));
    else if (method == QLatin1String("query") && args.size() > 1)
        params[QStringLiteral("kind")] = args[1];
    else if ((method == QLatin1String("open") || method == QLatin1String("save") ||
              method == QLatin1String("screenshot") ||
              method == QLatin1String("insertstep") ||
              method == QLatin1String("export")) &&
             args.size() > 1) {
        params[QStringLiteral("path")] = args[1];
        // `screenshot PATH clean` = 2D capture without overlay decorations
        // (grid/axes/crosshair) — geometry only, for reference image diffs.
        if (method == QLatin1String("screenshot") && args.size() > 2 &&
            args[2] == QLatin1String("clean"))
            params[QStringLiteral("overlays")] = false;
        // `export OUT.gts <layer>` = one fab layer by name (else the layer is
        // resolved from the extension; a directory path exports the whole kit).
        if (method == QLatin1String("export") && args.size() > 2)
            params[QStringLiteral("layer")] = args[2];
    }
    else if (method == QLatin1String("view3d") && args.size() > 1)
        params[QStringLiteral("on")] = args[1] != QLatin1String("off");
    else if (method == QLatin1String("viewdir") && args.size() > 1)
        params[QStringLiteral("name")] = args[1]; // TOP/FRONT/.../ISO
    else if (method == QLatin1String("pick3d") && args.size() > 2) {
        params[QStringLiteral("x")] = args[1].toInt();
        params[QStringLiteral("y")] = args[2].toInt();
    }

    QLocalSocket socket;
    socket.connectToServer(QStringLiteral("vikicad"));
    if (!socket.waitForConnected(2000))
        return emitError(QStringLiteral("E_CONNECT"),
                         QStringLiteral("no running VikiCAD GUI (socket 'vikicad')"));
    const QJsonObject req{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                          {QStringLiteral("id"), 1},
                          {QStringLiteral("method"), method},
                          {QStringLiteral("params"), params}};
    socket.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
    socket.flush();
    // Large replies (e.g. query entities with BREP solids) arrive in several
    // chunks: keep reading until the full newline-terminated line is buffered,
    // otherwise a partial line parses as an empty JSON object.
    while (!socket.canReadLine())
        if (!socket.waitForReadyRead(10000))
            return emitError(QStringLiteral("E_TIMEOUT"), QStringLiteral("no response"));
    const QJsonObject resp = QJsonDocument::fromJson(socket.readLine()).object();
    if (resp.contains(QStringLiteral("error")))
        return emitError(QStringLiteral("E_RPC"),
                         resp[QStringLiteral("error")]
                             .toObject()[QStringLiteral("message")]
                             .toString());
    return emitOk(resp[QStringLiteral("result")].toObject());
}

} // namespace

int main(int argc, char** argv)
{
    // Headless but font-capable (text metrics, PDF plotting).
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QStringList args = QGuiApplication::arguments();
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
    if (verb == QLatin1String("import"))
        return cmdImport(args);
    if (verb == QLatin1String("export"))
        return cmdExport(args);
    if (verb == QLatin1String("connect"))
        return cmdConnect(args);

    return emitError(QStringLiteral("E_UNKNOWN_VERB"),
                     QStringLiteral("unknown verb: %1").arg(verb));
}
