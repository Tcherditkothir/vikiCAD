#include "DxfExporter.h"

#include <drw_interface.h>
#include <libdxfrw.h>

#include "doc/Entities.h"
#include "doc/EntitiesEx.h"

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

    // Required but unused callbacks.
    void writeBlocks() override {}
    void writeBlockRecords() override {}
    void writeLTypes() override {}
    void writeTextstyles() override {}
    void writeVports() override {}
    void writeDimstyles() override {}
    void writeObjects() override {}
    void writeAppId() override {}

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
                for (const PolyVertex& v : pl->vertices())
                    out.addVertex(DRW_Vertex(v.pos.x, v.pos.y, 0.0, v.bulge));
                m_rw.writePolyline(&out);
            } else {
                DRW_LWPolyline out;
                applyCommon(e, out);
                out.flags = pl->isClosed() ? 1 : 0;
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
        } else {
            skip(e.typeName());
            return;
        }
        ++exported;
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
