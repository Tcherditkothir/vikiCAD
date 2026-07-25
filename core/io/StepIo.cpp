#include "StepIo.h"

#include <algorithm>
#include <cmath>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <BRep_Builder.hxx>
#include <Quantity_Color.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopLoc_Location.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_ColorType.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <Interface_InterfaceModel.hxx>
#include <StepBasic_ProductDefinition.hxx>
#include <StepRepr_CharacterizedDefinition.hxx>
#include <StepRepr_DescriptiveRepresentationItem.hxx>
#include <StepRepr_HArray1OfRepresentationItem.hxx>
#include <StepRepr_PropertyDefinition.hxx>
#include <StepRepr_PropertyDefinitionRepresentation.hxx>
#include <StepRepr_Representation.hxx>
#include <StepRepr_RepresentationContext.hxx>
#include <StepRepr_RepresentedDefinition.hxx>
#include <TCollection_HAsciiString.hxx>
#include <XSControl_WorkSession.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include "doc/StickyNote.h"
#include "io/OcctMessages.h"
#include "io/QueryJson.h"
#include "solid/SolidEntity.h"

namespace viki {

namespace {

QString sidecarPath(const QString& stepPath)
{
    return stepPath + QStringLiteral(".vikinotes.json");
}

// TDataStd_Name on an XCAF label — the STEP product/part name.
QString labelName(const TDF_Label& label)
{
    Handle(TDataStd_Name) attr;
    if (label.IsNull() || !label.FindAttribute(TDataStd_Name::GetID(), attr))
        return {};
    const TCollection_ExtendedString& ext = attr->Get();
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(ext.ToExtString()),
                              ext.Length())
        .trimmed();
}

// Colour for a leaf part. XCAF splits colour by role: Surf paints faces, Curv
// paints edges, Gen is the catch-all. Surf is what a solid shows, so it wins;
// Gen is the fallback. Looked up on the LABEL and, failing that, on the shape
// (some writers style the shape without labelling it).
bool leafColor(const Handle(XCAFDoc_ColorTool)& colorTool, const TDF_Label& label,
               const TopoDS_Shape& shape, Quantity_ColorRGBA& out)
{
    if (colorTool.IsNull())
        return false;
    for (const XCAFDoc_ColorType type : {XCAFDoc_ColorSurf, XCAFDoc_ColorGen}) {
        if (!label.IsNull() && XCAFDoc_ColorTool::GetColor(label, type, out))
            return true;
    }
    for (const XCAFDoc_ColorType type : {XCAFDoc_ColorSurf, XCAFDoc_ColorGen}) {
        if (!shape.IsNull() && colorTool->GetColor(shape, type, out))
            return true;
    }
    return false;
}

// Quantity_Color holds LINEAR RGB; ColorSpec::rgb is 8-bit sRGB (what the UI
// and the DXF/OBJ writers use). Ask OCCT for sRGB components rather than
// scaling the linear values by 255 — a mid grey would come out visibly dark.
uint32_t toRgb24(const Quantity_Color& c)
{
    Standard_Real r = 0, g = 0, b = 0;
    c.Values(r, g, b, Quantity_TOC_sRGB);
    const auto q = [](Standard_Real v) {
        return uint32_t(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
    };
    return (q(r) << 16) | (q(g) << 8) | q(b);
}

// Inverse of toRgb24: 8-bit sRGB back to a Quantity_Color. Same space tag on
// both sides, so export(import(f)) reproduces f's colours exactly.
Quantity_Color fromRgb24(uint32_t rgb)
{
    return Quantity_Color(double((rgb >> 16) & 0xFF) / 255.0,
                          double((rgb >> 8) & 0xFF) / 255.0,
                          double(rgb & 0xFF) / 255.0, Quantity_TOC_sRGB);
}

// One XCAF label -> zero or more SolidEntity in `doc`.
//
// Recurses through assemblies, composing each component's placement into `loc`
// so an instanced part lands where the assembly puts it. `parentName` carries
// the enclosing assembly's name down, so a leaf with no name of its own still
// gets a useful component tag.
void addLabelToDocument(Document& doc, const TDF_Label& label,
                        const Handle(XCAFDoc_ShapeTool)& shapeTool,
                        const Handle(XCAFDoc_ColorTool)& colorTool,
                        const TopLoc_Location& loc, const QString& parentName,
                        int& solids, StepResult& result)
{
    if (label.IsNull())
        return;

    QString name = labelName(label);
    if (name.isEmpty())
        name = parentName;

    if (XCAFDoc_ShapeTool::IsAssembly(label)) {
        TDF_LabelSequence components;
        XCAFDoc_ShapeTool::GetComponents(label, components);
        for (int i = 1; i <= components.Length(); ++i) {
            const TDF_Label component = components.Value(i);
            // A component label is a *reference*: its own location times the
            // referenced part's content.
            const TopLoc_Location childLoc =
                loc * XCAFDoc_ShapeTool::GetLocation(component);
            TDF_Label referred;
            if (XCAFDoc_ShapeTool::GetReferredShape(component, referred)) {
                // Name from the instance if it has one, else from the part.
                const QString instName = labelName(component);
                addLabelToDocument(doc, referred, shapeTool, colorTool, childLoc,
                                   instName.isEmpty() ? name : instName, solids,
                                   result);
            } else {
                addLabelToDocument(doc, component, shapeTool, colorTool, childLoc,
                                   name, solids, result);
            }
        }
        return;
    }

    const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(label);
    if (shape.IsNull())
        return;

    // Part-level colour, used only as the fallback. Real exporters usually
    // style each SOLID rather than the part: SolidWorks writes
    // STYLED_ITEM -> MANIFOLD_SOLID_BREP, one per body. Those land on XCAF
    // sub-shape labels, so the per-solid lookup below is the one that matters.
    Quantity_ColorRGBA partRgba;
    const bool partHasColor = leafColor(colorTool, label, shape, partRgba);

    // NOTE: colours and names are looked up on the UN-LOCATED solid, because
    // that is the shape XCAF indexed. Each solid is moved into place after.
    int here = 0;
    for (TopExp_Explorer exp(shape, TopAbs_SOLID); exp.More(); exp.Next()) {
        TopoDS_Shape solid = exp.Current();

        QString solidName;
        Quantity_ColorRGBA rgba = partRgba;
        bool hasColor = partHasColor;

        TDF_Label subLabel;
        if (!shapeTool.IsNull() && shapeTool->FindSubShape(label, solid, subLabel)
            && !subLabel.IsNull()) {
            solidName = labelName(subLabel);
            Quantity_ColorRGBA sub;
            if (leafColor(colorTool, subLabel, solid, sub)) {
                rgba = sub;
                hasColor = true;
            }
        } else {
            // No sub-shape label: the colour tool can still resolve a style
            // attached straight to this shape.
            Quantity_ColorRGBA direct;
            if (leafColor(colorTool, TDF_Label(), solid, direct)) {
                rgba = direct;
                hasColor = true;
            }
        }

        if (!loc.IsIdentity())
            solid.Move(loc);

        auto entity = std::make_unique<SolidEntity>(solid);
        // A body's own name ("SHIELD", "LeftLight") tells the parts apart in the
        // assembly tree; the part name only repeats itself across every body.
        const QString bestName = solidName.isEmpty() ? name : solidName;
        if (!bestName.isEmpty()) {
            entity->component = bestName;
            ++result.named;
        }
        if (hasColor) {
            entity->setColor(ColorSpec{/*byLayer=*/false, toRgb24(rgba.GetRGB())});
            // STEP alpha is opacity; SolidEntity::transparency is its complement.
            entity->transparency = 1.0 - double(rgba.Alpha());
            ++result.colored;
        }
        doc.restoreEntity(std::move(entity), doc.nextId());
        doc.setNextId(doc.nextId() + 1);
        ++here;
    }
    solids += here;
}
} // namespace

StepResult exportStep(const Document& doc, const QString& path)
{
    StepResult result;
    silenceOcctMessages();

    // Build an XCAF document first: that is the only way STEP colours and part
    // names get written. A plain STEPControl_Writer — what this used to be —
    // emits geometry only, so a coloured model exported and reimported came back
    // grey even after the reader learned to read colour.
    Handle(TDocStd_Document) xdoc;
    XCAFApp_Application::GetApplication()->NewDocument(
        TCollection_ExtendedString("MDTV-XCAF"), xdoc);
    Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(xdoc->Main());
    Handle(XCAFDoc_ColorTool) colorTool = XCAFDoc_DocumentTool::ColorTool(xdoc->Main());

    int solids = 0;
    for (const EntityId id : doc.drawOrder()) {
        const auto* solid = dynamic_cast<const SolidEntity*>(doc.entity(id));
        if (!solid || solid->shape().IsNull())
            continue;
        const TDF_Label label = shapeTool->AddShape(solid->shape(), /*makeAssembly=*/
                                                    Standard_False);
        if (label.IsNull()) {
            result.error = QStringLiteral("STEP transfer failed for solid %1").arg(id);
            return result;
        }
        if (!solid->component.isEmpty())
            TDataStd_Name::Set(label,
                               TCollection_ExtendedString(
                                   solid->component.toUtf8().constData(), Standard_True));
        // ByLayer means "no colour of its own": leave it unstyled rather than
        // baking today's layer colour into the file.
        if (!solid->color().byLayer) {
            const double alpha = 1.0 - std::clamp(solid->transparency, 0.0, 1.0);
            colorTool->SetColor(solid->shape(),
                                Quantity_ColorRGBA(fromRgb24(solid->color().rgb),
                                                   Standard_ShortReal(alpha)),
                                XCAFDoc_ColorSurf);
        }
        ++solids;
    }
    if (solids == 0) {
        result.error = QStringLiteral("no solids to export (EXTRUDE/REVOLVE first)");
        return result;
    }

    STEPCAFControl_Writer writer;
    writer.SetColorMode(Standard_True);
    writer.SetNameMode(Standard_True);
    if (!writer.Transfer(xdoc, STEPControl_AsIs)) {
        result.error = QStringLiteral("STEP transfer failed");
        return result;
    }
    // Plan A (flag-gated): inject notes as AP242-style user-defined
    // attributes (PROPERTY_DEFINITION -> DESCRIPTIVE_REPRESENTATION_ITEM on
    // the product definition). The sidecar below remains the safety net.
    if (qEnvironmentVariableIntValue("VIKICAD_STEP_UDA") == 1) {
        const QJsonArray notesArr = queryjson::notesJson(doc);
        // The CAF writer wraps a STEPControl_Writer; the model to inject into is
        // that inner writer's.
        Handle(Interface_InterfaceModel) model = writer.ChangeWriter().WS()->Model();
        Handle(StepBasic_ProductDefinition) pd;
        Handle(StepRepr_RepresentationContext) repCtx;
        for (int i = 1; i <= model->NbEntities(); ++i) {
            if (pd.IsNull())
                pd = Handle(StepBasic_ProductDefinition)::DownCast(model->Value(i));
            if (repCtx.IsNull())
                repCtx =
                    Handle(StepRepr_RepresentationContext)::DownCast(model->Value(i));
            if (!pd.IsNull() && !repCtx.IsNull())
                break;
        }
        if (!pd.IsNull() && !repCtx.IsNull()) {
            for (const QJsonValue& nv : notesArr) {
                const QByteArray payload =
                    QJsonDocument(nv.toObject()).toJson(QJsonDocument::Compact);
                Handle(StepRepr_DescriptiveRepresentationItem) item =
                    new StepRepr_DescriptiveRepresentationItem();
                item->Init(new TCollection_HAsciiString("VIKI_STICKYNOTE"),
                           new TCollection_HAsciiString(payload.constData()));
                Handle(StepRepr_HArray1OfRepresentationItem) items =
                    new StepRepr_HArray1OfRepresentationItem(1, 1);
                items->SetValue(1, item);
                Handle(StepRepr_Representation) rep = new StepRepr_Representation();
                rep->Init(new TCollection_HAsciiString("sticky note"), items, repCtx);
                Handle(StepRepr_PropertyDefinition) prop =
                    new StepRepr_PropertyDefinition();
                StepRepr_CharacterizedDefinition cd;
                cd.SetValue(pd);
                prop->Init(new TCollection_HAsciiString("user defined attribute"),
                           Standard_True,
                           new TCollection_HAsciiString("VikiCAD sticky note"), cd);
                Handle(StepRepr_PropertyDefinitionRepresentation) pdr =
                    new StepRepr_PropertyDefinitionRepresentation();
                StepRepr_RepresentedDefinition rd;
                rd.SetValue(prop);
                pdr->Init(rd, rep);
                model->AddWithRefs(pdr);
            }
        }
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

    // XCAF reader: geometry AND presentation. SetColorMode/SetNameMode are off
    // by default, and without them this degrades silently into the old
    // geometry-only behaviour.
    STEPCAFControl_Reader reader;
    reader.SetColorMode(Standard_True);
    reader.SetNameMode(Standard_True);
    reader.SetLayerMode(Standard_True);

    Handle(TDocStd_Document) xdoc;
    XCAFApp_Application::GetApplication()->NewDocument(
        TCollection_ExtendedString("MDTV-XCAF"), xdoc);
    if (!reader.Perform(path.toUtf8().constData(), xdoc)) {
        result.error = QStringLiteral("cannot read STEP file %1").arg(path);
        return result;
    }

    Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(xdoc->Main());
    Handle(XCAFDoc_ColorTool) colorTool = XCAFDoc_DocumentTool::ColorTool(xdoc->Main());
    if (shapeTool.IsNull()) {
        result.error = QStringLiteral("no shape structure in %1").arg(path);
        return result;
    }

    TDF_LabelSequence roots;
    shapeTool->GetFreeShapes(roots);
    if (roots.IsEmpty()) {
        result.error = QStringLiteral("no shapes in %1").arg(path);
        return result;
    }

    outDoc = std::make_unique<Document>();

    // Walk the assembly tree so instance placements are applied and every leaf
    // keeps its own name and colour, then explode each leaf into solids exactly
    // as before — one SolidEntity per TopAbs_SOLID.
    int solids = 0;
    for (int i = 1; i <= roots.Length(); ++i)
        addLabelToDocument(*outDoc, roots.Value(i), shapeTool, colorTool,
                           TopLoc_Location(), QString(), solids, result);

    if (solids == 0) {
        // No SOLID anywhere (a shell-only or surface-only STEP): keep the whole
        // thing as one entity rather than opening an empty document.
        const TopoDS_Shape all = shapeTool->GetShape(roots.Value(1));
        if (all.IsNull()) {
            outDoc.reset();
            result.error = QStringLiteral("no usable shape in %1").arg(path);
            return result;
        }
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
