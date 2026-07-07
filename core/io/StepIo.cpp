#include "StepIo.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <BRep_Builder.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include "doc/StickyNote.h"
#include "io/QueryJson.h"
#include "solid/SolidEntity.h"

namespace viki {

namespace {

// OCCT prints transfer statistics on stdout, which would corrupt the CLI's
// JSON output. Silence the default messenger once.
void silenceOcctMessages()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream));
}

QString sidecarPath(const QString& stepPath)
{
    return stepPath + QStringLiteral(".vikinotes.json");
}
} // namespace

StepResult exportStep(const Document& doc, const QString& path)
{
    StepResult result;
    silenceOcctMessages();

    STEPControl_Writer writer;
    int solids = 0;
    for (const EntityId id : doc.drawOrder()) {
        const auto* solid = dynamic_cast<const SolidEntity*>(doc.entity(id));
        if (!solid || solid->shape().IsNull())
            continue;
        if (writer.Transfer(solid->shape(), STEPControl_AsIs) != IFSelect_RetDone) {
            result.error = QStringLiteral("STEP transfer failed for solid %1").arg(id);
            return result;
        }
        ++solids;
    }
    if (solids == 0) {
        result.error = QStringLiteral("no solids to export (EXTRUDE/REVOLVE first)");
        return result;
    }
    if (writer.Write(path.toUtf8().constData()) != IFSelect_RetDone) {
        result.error = QStringLiteral("cannot write %1").arg(path);
        return result;
    }

    // Sidecar notes: always written when notes exist (Plan B, reliable).
    const QJsonArray notes = queryjson::notesJson(doc);
    if (!notes.isEmpty()) {
        QFile f(sidecarPath(path));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                              {QStringLiteral("notes"), notes}})
                        .toJson(QJsonDocument::Indented));
            result.notes = notes.size();
        }
    }

    result.ok = true;
    result.solids = solids;
    return result;
}

StepResult importStep(const QString& path, std::unique_ptr<Document>& outDoc)
{
    StepResult result;
    silenceOcctMessages();

    STEPControl_Reader reader;
    if (reader.ReadFile(path.toUtf8().constData()) != IFSelect_RetDone) {
        result.error = QStringLiteral("cannot read STEP file %1").arg(path);
        return result;
    }
    reader.TransferRoots();
    const TopoDS_Shape all = reader.OneShape();
    if (all.IsNull()) {
        result.error = QStringLiteral("no shapes in %1").arg(path);
        return result;
    }

    outDoc = std::make_unique<Document>();

    // One SolidEntity per SOLID (fallback: the whole shape as one entity).
    int solids = 0;
    for (TopExp_Explorer exp(all, TopAbs_SOLID); exp.More(); exp.Next()) {
        outDoc->restoreEntity(std::make_unique<SolidEntity>(exp.Current()),
                              outDoc->nextId());
        outDoc->setNextId(outDoc->nextId() + 1);
        ++solids;
    }
    if (solids == 0) {
        outDoc->restoreEntity(std::make_unique<SolidEntity>(all), outDoc->nextId());
        outDoc->setNextId(outDoc->nextId() + 1);
        solids = 1;
    }

    // Sidecar notes back in.
    QFile f(sidecarPath(path));
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonArray notes =
            QJsonDocument::fromJson(f.readAll()).object()[QStringLiteral("notes")].toArray();
        const LayerId layer = outDoc->ensureLayer(
            QLatin1String(StickyNoteEntity::kLayerName), 0xE8C84A);
        outDoc->setLayerPrintable(layer, false);
        for (const QJsonValue& v : notes) {
            const QJsonObject o = v.toObject();
            auto note = std::make_unique<StickyNoteEntity>();
            note->text = o[QStringLiteral("text")].toString();
            note->author = o[QStringLiteral("author")].toString();
            note->created = o[QStringLiteral("created")].toString();
            note->modified = o[QStringLiteral("modified")].toString();
            const QJsonArray anchor = o[QStringLiteral("anchor")].toArray();
            note->anchor = {anchor.at(0).toDouble(), anchor.at(1).toDouble()};
            note->setLayerId(layer);
            outDoc->restoreEntity(std::move(note), outDoc->nextId());
            outDoc->setNextId(outDoc->nextId() + 1);
            ++result.notes;
        }
    }

    result.ok = true;
    result.solids = solids;
    return result;
}

} // namespace viki
