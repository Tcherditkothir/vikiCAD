#include "DxfExporter.h"

#include <drw_interface.h>
#include <libdxfrw.h>

#include <QJsonDocument>

#include "doc/Annotations.h"
#include "doc/ArrayEntity.h"
#include "doc/Block.h"
#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "doc/StickyNote.h"

namespace viki {
namespace {

int rgbToNearestAci(uint32_t rgb)
{
    const int r = int(rgb >> 16 & 0xFF), g = int(rgb >> 8 & 0xFF), b = int(rgb & 0xFF);
    int best = 7;
    long bestDist = std::numeric_limits<long>::max();
    for (int i = 1; i <= 255; ++i) {
        const unsigned char* c = DRW::dxfColors[i];
        const long dr = r - c[0], dg = g - c[1], db = b - c[2];
        const long d = dr * dr + dg * dg + db * db;
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

DRW::Version parseVersion(const QString& v)
{
    if (v == QLatin1String("R12")) return DRW::AC1009;
    if (v == QLatin1String("2000")) return DRW::AC1015;
    if (v == QLatin1String("2004")) return DRW::AC1018;
    if (v == QLatin1String("2007")) return DRW::AC1021;
    if (v == QLatin1String("2010")) return DRW::AC1024;
    if (v == QLatin1String("2018")) return DRW::AC1032;
    return DRW::AC1027; // 2013 default
}

class Writer : public DRW_Interface {
public:
    Writer(const Document& doc, dxfRW& rw, bool r12)
        : m_doc(doc), m_rw(rw), m_r12(r12) {}

    int exported = 0;
    int skipped = 0;
    QStringList skippedTypes;

    // ---- writer callbacks ----------------------------------------------------

    void writeHeader(DRW_Header& data) override
    {
        data.addInt("$INSUNITS",
                    m_doc.displayUnits() == DisplayUnits::Inches ? 1 : 4, 70);
    }

    void writeLayers() override
    {
        for (const Layer& l : m_doc.layers()) {
            DRW_Layer out;
            out.name = l.name.toStdString();
            const int aci = rgbToNearestAci(l.rgb);
            out.color = l.visible ? aci : -aci; // negative = off
            out.color24 = int(l.rgb);
            out.flags = l.locked ? 4 : 0;
            out.lineType = "CONTINUOUS";
            m_rw.writeLayer(&out);
        }
    }

    void writeEntities() override
    {
        for (const EntityId id : m_doc.drawOrder())
            if (const Entity* e = m_doc.entity(id))
                writeOne(*e);
    }

    void writeBlockRecords() override
    {
        for (const auto& blk : m_doc.blocks())
            m_rw.writeBlockRecord(blk->name.toStdString());
    }

    void writeBlocks() override
    {
        for (const auto& blk : m_doc.blocks()) {
            DRW_Block out;
            out.name = blk->name.toStdString();
            out.basePoint = {blk->basePoint.x, blk->basePoint.y, 0};
            m_rw.writeBlock(&out);
            for (const auto& e : blk->entities) {
                if (dynamic_cast<const AttDefEntity*>(e.get()))
                    continue; // ATTDEF has no libdxfrw writer — documented gap
                writeOne(*e);
                --exported; // block contents don't count as model entities
            }
        }
    }

    void writeAppId() override
    {
        DRW_AppId app;
        app.name = "VIKI_STICKYNOTE";
        m_rw.writeAppId(&app);
    }

    // Required but unused callbacks.
    void writeLTypes() override {}
    void writeTextstyles() override {}
    void writeVports() override {}
    void writeDimstyles() override {}
    void writeObjects() override {}

    // Reader-side callbacks — never used while writing.
    void addHeader(const DRW_Header*) override {}
    void addLType(const DRW_LType&) override {}
    void addLayer(const DRW_Layer&) override {}
    void addDimStyle(const DRW_Dimstyle&) override {}
    void addVport(const DRW_Vport&) override {}
    void addTextStyle(const DRW_Textstyle&) override {}
    void addAppId(const DRW_AppId&) override {}
    void addBlock(const DRW_Block&) override {}
    void setBlock(const int) override {}
    void endBlock() override {}
    void addPoint(const DRW_Point&) override {}
    void addLine(const DRW_Line&) override {}
    void addRay(const DRW_Ray&) override {}
    void addXline(const DRW_Xline&) override {}
    void addArc(const DRW_Arc&) override {}
    void addCircle(const DRW_Circle&) override {}
    void addEllipse(const DRW_Ellipse&) override {}
    void addLWPolyline(const DRW_LWPolyline&) override {}
    void addPolyline(const DRW_Polyline&) override {}
    void addSpline(const DRW_Spline*) override {}
    void addKnot(const DRW_Entity&) override {}
    void addInsert(const DRW_Insert&) override {}
    void addTrace(const DRW_Trace&) override {}
    void add3dFace(const DRW_3Dface&) override {}
    void addSolid(const DRW_Solid&) override {}
    void addMText(const DRW_MText&) override {}
    void addText(const DRW_Text&) override {}
    void addDimAlign(const DRW_DimAligned*) override {}
    void addDimLinear(const DRW_DimLinear*) override {}
    void addDimRadial(const DRW_DimRadial*) override {}
    void addDimDiametric(const DRW_DimDiametric*) override {}
    void addDimAngular(const DRW_DimAngular*) override {}
    void addDimAngular3P(const DRW_DimAngular3p*) override {}
    void addDimOrdinate(const DRW_DimOrdinate*) override {}
    void addLeader(const DRW_Leader*) override {}
    void addHatch(const DRW_Hatch*) override {}
    void addViewport(const DRW_Viewport&) override {}
    void addImage(const DRW_Image*) override {}
    void linkImage(const DRW_ImageDef*) override {}
    void addComment(const char*) override {}
    void addPlotSettings(const DRW_PlotSettings*) override {}

private:
    void applyCommon(const Entity& e, DRW_Entity& out)
    {
        const Layer* layer = m_doc.layer(e.layerId());
        out.layer = layer ? layer->name.toStdString() : std::string("0");
        if (e.color().byLayer) {
            out.color = 256; // bylayer
        } else {
            out.color = rgbToNearestAci(e.color().rgb);
            out.color24 = int(e.color().rgb);
        }
    }

    void skip(const char* type)
    {
        ++skipped;
        const QString t = QLatin1String(type);
        if (!skippedTypes.contains(t))
            skippedTypes.append(t);
    }

    void writeOne(const Entity& e)
    {
        if (const auto* line = dynamic_cast<const LineEntity*>(&e)) {
            DRW_Line out;
            applyCommon(e, out);
            out.basePoint = {line->p1().x, line->p1().y, 0};
            out.secPoint = {line->p2().x, line->p2().y, 0};
            m_rw.writeLine(&out);
        } else if (const auto* pt = dynamic_cast<const PointEntity*>(&e)) {
            DRW_Point out;
            applyCommon(e, out);
            out.basePoint = {pt->position().x, pt->position().y, 0};
            m_rw.writePoint(&out);
        } else if (const auto* c = dynamic_cast<const CircleEntity*>(&e)) {
            DRW_Circle out;
            applyCommon(e, out);
            out.basePoint = {c->center().x, c->center().y, 0};
            out.radious = c->radius();
            m_rw.writeCircle(&out);
        } else if (const auto* arc = dynamic_cast<const ArcEntity*>(&e)) {
            DRW_Arc out;
            applyCommon(e, out);
            out.basePoint = {arc->center().x, arc->center().y, 0};
            out.radious = arc->radius();
            out.staangle = arc->startAngle();
            out.endangle = arc->startAngle() + arc->sweep();
            m_rw.writeArc(&out);
        } else if (const auto* el = dynamic_cast<const EllipseEntity*>(&e)) {
            DRW_Ellipse out;
            applyCommon(e, out);
            out.basePoint = {el->center().x, el->center().y, 0};
            out.secPoint = {el->majorAxis().x, el->majorAxis().y, 0};
            out.ratio = el->ratio();
            out.staparam = el->isFull() ? 0.0 : el->startParam();
            out.endparam = el->isFull() ? 2.0 * M_PI : el->endParam();
            m_rw.writeEllipse(&out);
        } else if (const auto* pl = dynamic_cast<const PolylineEntity*>(&e)) {
            if (m_r12) {
                // R12 has no LWPOLYLINE; libdxfrw silently drops it there,
                // so emit a legacy POLYLINE instead.
                DRW_Polyline out;
                applyCommon(e, out);
                out.flags = pl->isClosed() ? 1 : 0;
                // Constant stroke width (a Gerber trace): legacy default
                // start/end widths, codes 40/41.
                out.defstawidth = out.defendwidth = pl->width();
                for (const PolyVertex& v : pl->vertices())
                    out.addVertex(DRW_Vertex(v.pos.x, v.pos.y, 0.0, v.bulge));
                m_rw.writePolyline(&out);
            } else {
                DRW_LWPolyline out;
                applyCommon(e, out);
                out.flags = pl->isClosed() ? 1 : 0;
                // Constant width, code 43 — how a Gerber trace crosses the
                // DXF bridge (round-tripped by our importer).
                out.width = pl->width();
                for (const PolyVertex& v : pl->vertices())
                    out.addVertex(DRW_Vertex2D(v.pos.x, v.pos.y, v.bulge));
                m_rw.writeLWPolyline(&out);
            }
        } else if (const auto* sp = dynamic_cast<const SplineEntity*>(&e)) {
            DRW_Spline out;
            applyCommon(e, out);
            out.degree = sp->degree;
            out.flags = sp->closed ? 1 : 0;
            for (const Vec2d& cp : sp->controlPoints)
                out.controllist.push_back(std::make_shared<DRW_Coord>(cp.x, cp.y, 0));
            for (const double k : sp->knots)
                out.knotslist.push_back(k);
            for (const double w : sp->weights)
                out.weightlist.push_back(w);
            for (const Vec2d& f : sp->fitPoints)
                out.fitlist.push_back(std::make_shared<DRW_Coord>(f.x, f.y, 0));
            out.ncontrol = int(out.controllist.size());
            out.nknots = int(out.knotslist.size());
            out.nfit = int(out.fitlist.size());
            m_rw.writeSpline(&out);
        } else if (const auto* xl = dynamic_cast<const XLineEntity*>(&e)) {
            DRW_Xline out;
            applyCommon(e, out);
            out.basePoint = {xl->basePoint().x, xl->basePoint().y, 0};
            out.secPoint = {xl->direction().x, xl->direction().y, 0};
            m_rw.writeXline(&out);
        } else if (const auto* txt = dynamic_cast<const TextEntity*>(&e)) {
            const QStringList lines = txt->text().split(QLatin1Char('\n'));
            if (lines.size() == 1 || m_r12) {
                // TEXT records (one per line in R12, which has no MTEXT).
                // The vertical alignment is baked into per-line baseline
                // anchors; the horizontal one maps to codes 72/11.
                const double y0 = TextEntity::firstBaselineY(
                    txt->vAlign, txt->height(), txt->lineSpacing, lines.size());
                const Vec2d base =
                    txt->position() + Vec2d{0, y0}.rotated(txt->rotation());
                const Vec2d down =
                    Vec2d{0, -txt->lineSpacing * txt->height()}.rotated(
                        txt->rotation());
                for (int i = 0; i < lines.size(); ++i) {
                    DRW_Text out;
                    applyCommon(e, out);
                    const Vec2d p = base + down * double(i);
                    out.basePoint = {p.x, p.y, 0};
                    out.secPoint = out.basePoint;
                    out.height = txt->height();
                    out.angle = txt->rotation() * 180.0 / M_PI;
                    if (txt->hAlign == TextHAlign::Center)
                        out.alignH = DRW_Text::HCenter;
                    else if (txt->hAlign == TextHAlign::Right)
                        out.alignH = DRW_Text::HRight;
                    out.text = lines[i].toStdString();
                    m_rw.writeText(&out);
                }
            } else {
                DRW_MText out;
                applyCommon(e, out);
                // MTEXT has no baseline attachment: Baseline exports as a
                // Top row with the anchor lifted one cap height so the
                // geometry survives the round trip exactly.
                Vec2d anchor = txt->position();
                int row = 0; // Top
                switch (txt->vAlign) {
                case TextVAlign::Baseline:
                    anchor = anchor + Vec2d{0, txt->height()}.rotated(txt->rotation());
                    break;
                case TextVAlign::Top: break;
                case TextVAlign::Middle: row = 1; break;
                case TextVAlign::Bottom: row = 2; break;
                }
                int col = 0; // Left
                if (txt->hAlign == TextHAlign::Center)
                    col = 1;
                else if (txt->hAlign == TextHAlign::Right)
                    col = 2;
                out.textgen = row * 3 + col + 1; // attachment point, code 71
                out.interlin = txt->lineSpacing * 3.0 / 5.0; // code 44
                out.basePoint = {anchor.x, anchor.y, 0};
                out.secPoint = out.basePoint;
                out.height = txt->height();
                out.angle = txt->rotation(); // MTEXT code 50 is radians (spec)
                if (txt->columnWidth > 0.0)
                    out.widthscale = txt->columnWidth; // reference width, code 41
                QString content = txt->text();
                content.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
                content.replace(QLatin1Char('{'), QLatin1String("\\{"));
                content.replace(QLatin1Char('}'), QLatin1String("\\}"));
                content.replace(QLatin1Char('\n'), QLatin1String("\\P"));
                out.text = content.toStdString();
                m_rw.writeMText(&out);
            }
        } else if (const auto* dim = dynamic_cast<const DimensionEntity*>(&e)) {
            if (m_r12) { // DIMENSION writing needs R13+ in libdxfrw
                skip("dimension-r12");
                return;
            }
            writeDim(*dim, e);
        } else if (const auto* leader = dynamic_cast<const LeaderEntity*>(&e)) {
            if (m_r12) {
                skip("leader-r12");
                return;
            }
            DRW_Leader out;
            applyCommon(e, out);
            out.arrow = 1;
            for (const Vec2d& p : leader->points)
                out.vertexlist.push_back(std::make_shared<DRW_Coord>(p.x, p.y, 0));
            out.vertnum = int(leader->points.size());
            m_rw.writeLeader(&out);
            if (!leader->text.isEmpty()) {
                DRW_MText t;
                applyCommon(e, t);
                const Vec2d at = leader->points.back();
                t.basePoint = {at.x, at.y, 0};
                t.secPoint = t.basePoint;
                t.height = 3.5;
                t.text = leader->text.toStdString();
                m_rw.writeMText(&t);
            }
        } else if (const auto* ins = dynamic_cast<const InsertEntity*>(&e)) {
            DRW_Insert out;
            applyCommon(e, out);
            out.name = ins->blockName.toStdString();
            out.basePoint = {ins->position.x, ins->position.y, 0};
            out.xscale = ins->scale;
            out.yscale = ins->effScaleY();
            out.zscale = 1.0;
            out.angle = ins->rotation; // radians; writeInsert emits degrees
            m_rw.writeInsert(&out);
        } else if (const auto* arr = dynamic_cast<const ArrayEntity*>(&e)) {
            // Arrays export flattened (associativity is native-format only).
            for (const auto& item : arr->materialize()) {
                writeOne(*item);
                --exported;
            }
            skippedTypes.removeAll(QStringLiteral("array-flattened"));
            skippedTypes.append(QStringLiteral("array-flattened"));
        } else if (const auto* note = dynamic_cast<const StickyNoteEntity*>(&e)) {
            DRW_Point out;
            applyCommon(e, out);
            out.basePoint = {note->anchor.x, note->anchor.y, 0};
            out.extData.push_back(std::make_shared<DRW_Variant>(
                1001, UTF8STRING("VIKI_STICKYNOTE")));
            QJsonObject header{{QStringLiteral("v"), 1},
                               {QStringLiteral("author"), note->author},
                               {QStringLiteral("created"), note->created},
                               {QStringLiteral("modified"), note->modified}};
            out.extData.push_back(std::make_shared<DRW_Variant>(
                1000, UTF8STRING(QJsonDocument(header)
                                     .toJson(QJsonDocument::Compact)
                                     .constData())));
            // Text chunked to stay under the 255-byte group limit.
            const QByteArray utf8 = note->text.toUtf8();
            for (int i = 0; i < utf8.size(); i += 240)
                out.extData.push_back(std::make_shared<DRW_Variant>(
                    1000, UTF8STRING(utf8.mid(i, 240).constData())));
            m_rw.writePoint(&out);
        } else if (const auto* hatch = dynamic_cast<const HatchEntity*>(&e)) {
            if (m_r12) {
                skip("hatch-r12");
                return;
            }
            DRW_Hatch out;
            applyCommon(e, out);
            const bool solid =
                hatch->pattern.compare(QLatin1String("SOLID"), Qt::CaseInsensitive) == 0;
            out.solid = solid ? 1 : 0;
            out.name = solid ? std::string("SOLID")
                             : hatch->pattern.toUpper().toStdString();
            out.scale = hatch->scale;
            out.angle = hatch->angle * 180.0 / M_PI;
            out.hstyle = 0;
            for (const auto& ring : hatch->rings) {
                // Edge-type loop of LINE segments: the vendored writer has
                // no polyline-loop support ("writeme" upstream).
                auto loop = std::make_shared<DRW_HatchLoop>(1); // external
                const size_t n = ring.size();
                for (size_t i = 0; i < n; ++i) {
                    auto edge = std::make_shared<DRW_Line>();
                    edge->basePoint = {ring[i].x, ring[i].y, 0};
                    edge->secPoint = {ring[(i + 1) % n].x, ring[(i + 1) % n].y, 0};
                    loop->objlist.push_back(edge);
                }
                loop->update();
                out.appendLoop(loop);
            }
            out.loopsnum = int(hatch->rings.size());
            m_rw.writeHatch(&out);
        } else {
            skip(e.typeName());
            return;
        }
        ++exported;
    }

    void writeDim(const DimensionEntity& dim, const Entity& e)
    {
        using K = DimensionEntity::Kind;
        switch (dim.kind) {
        case K::Linear: {
            DRW_DimLinear out;
            applyCommon(e, out);
            out.setDef1Point({dim.a.x, dim.a.y, 0});
            out.setDef2Point({dim.b.x, dim.b.y, 0});
            out.setDimPoint({dim.pos.x, dim.pos.y, 0});
            out.setAngle(dim.axis.angle() * 180.0 / M_PI);
            out.setTextPoint({dim.pos.x, dim.pos.y, 0});
            if (!dim.textOverride.isEmpty())
                out.setText(dim.textOverride.toStdString());
            m_rw.writeDimension(&out);
            break;
        }
        case K::Aligned: {
            DRW_DimAligned out;
            applyCommon(e, out);
            out.setDef1Point({dim.a.x, dim.a.y, 0});
            out.setDef2Point({dim.b.x, dim.b.y, 0});
            out.setDimPoint({dim.pos.x, dim.pos.y, 0});
            out.setTextPoint({dim.pos.x, dim.pos.y, 0});
            m_rw.writeDimension(&out);
            break;
        }
        case K::Radius: {
            DRW_DimRadial out;
            applyCommon(e, out);
            out.setCenterPoint({dim.a.x, dim.a.y, 0});
            out.setDiameterPoint({dim.b.x, dim.b.y, 0});
            out.setTextPoint({dim.pos.x, dim.pos.y, 0});
            m_rw.writeDimension(&out);
            break;
        }
        case K::Diameter: {
            DRW_DimDiametric out;
            applyCommon(e, out);
            const Vec2d dir = (dim.b - dim.a).normalized();
            const double r = dim.a.distanceTo(dim.b);
            const Vec2d opposite = dim.a - dir * r;
            out.setDiameter1Point({dim.b.x, dim.b.y, 0});
            out.setDiameter2Point({opposite.x, opposite.y, 0});
            out.setTextPoint({dim.pos.x, dim.pos.y, 0});
            m_rw.writeDimension(&out);
            break;
        }
        case K::Angular: {
            DRW_DimAngular3p out;
            applyCommon(e, out);
            out.SetVertexPoint({dim.a.x, dim.a.y, 0});
            out.setFirstLine({dim.b.x, dim.b.y, 0});
            out.setSecondLine({dim.c.x, dim.c.y, 0});
            out.setDimPoint({dim.pos.x, dim.pos.y, 0});
            out.setTextPoint({dim.pos.x, dim.pos.y, 0});
            m_rw.writeDimension(&out);
            break;
        }
        }
    }

    const Document& m_doc;
    dxfRW& m_rw;
    bool m_r12;
};

} // namespace

DxfExportResult exportDxf(const Document& doc, const QString& path, const QString& version)
{
    DxfExportResult result;
    dxfRW rw(path.toUtf8().constData());
    Writer writer(doc, rw, parseVersion(version) == DRW::AC1009);
    if (!rw.write(&writer, parseVersion(version), /*binary=*/false)) {
        result.error = QStringLiteral("failed to write DXF: %1").arg(path);
        return result;
    }
    result.ok = true;
    result.exported = writer.exported;
    result.skipped = writer.skipped;
    result.skippedTypes = writer.skippedTypes;
    return result;
}

} // namespace viki
