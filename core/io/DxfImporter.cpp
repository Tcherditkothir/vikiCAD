#include "DxfImporter.h"

#include <drw_interface.h>
#include <libdxfrw.h>

#include "doc/Entities.h"
#include "doc/EntitiesEx.h"
#include "geom/GeomUtil.h"

namespace viki {
namespace {

// ACI (AutoCAD Color Index) -> RGB using libdxfrw's own table.
uint32_t aciToRgb(int aci)
{
    if (aci < 0 || aci > 255)
        return 0xFFFFFF;
    if (aci == 7)
        return 0xFFFFFF; // "white/black" — white on our dark canvas
    const unsigned char* c = DRW::dxfColors[aci];
    return uint32_t(c[0]) << 16 | uint32_t(c[1]) << 8 | uint32_t(c[2]);
}

class Builder : public DRW_Interface {
public:
    Builder()
        : doc(std::make_unique<Document>())
    {
    }

    std::unique_ptr<Document> doc;
    int imported = 0;
    int skipped = 0;
    QStringList skippedTypes;
    bool inBlock = false;

    // ---- helpers ------------------------------------------------------------

    void skip(const char* type)
    {
        ++skipped;
        const QString t = QLatin1String(type);
        if (!skippedTypes.contains(t))
            skippedTypes.append(t);
    }

    void place(std::unique_ptr<Entity> e, const DRW_Entity& src)
    {
        if (inBlock) {
            // Block definitions are M5 scope; their entities are not model
            // space content, so they are dropped (and counted once).
            skip("block-content");
            return;
        }
        const LayerId layer =
            doc->ensureLayer(QString::fromStdString(src.layer), 0xFFFFFF);
        e->setLayerId(layer);
        ColorSpec color; // default bylayer
        if (src.color24 >= 0) {
            color.byLayer = false;
            color.rgb = uint32_t(src.color24);
        } else if (src.color != 256 && src.color != 0) { // 256=bylayer, 0=byblock
            color.byLayer = false;
            color.rgb = aciToRgb(src.color);
        }
        e->setColor(color);
        doc->restoreEntity(std::move(e), doc->nextId());
        doc->setNextId(doc->nextId() + 1);
        ++imported;
    }

    // ---- tables -------------------------------------------------------------

    void addLayer(const DRW_Layer& data) override
    {
        const bool frozen = data.flags & 1;
        const bool locked = data.flags & 4;
        // Negative color = layer off in DXF convention.
        const bool off = data.color < 0;
        const int aci = std::abs(data.color);
        doc->ensureLayer(QString::fromStdString(data.name), aciToRgb(aci),
                         !(frozen || off), locked);
    }

    // ---- entities -----------------------------------------------------------

    void addPoint(const DRW_Point& data) override
    {
        place(std::make_unique<PointEntity>(Vec2d{data.basePoint.x, data.basePoint.y}), data);
    }

    void addLine(const DRW_Line& data) override
    {
        place(std::make_unique<LineEntity>(Vec2d{data.basePoint.x, data.basePoint.y},
                                           Vec2d{data.secPoint.x, data.secPoint.y}),
              data);
    }

    void addXline(const DRW_Xline& data) override
    {
        place(std::make_unique<XLineEntity>(Vec2d{data.basePoint.x, data.basePoint.y},
                                            Vec2d{data.secPoint.x, data.secPoint.y}),
              data);
    }

    void addRay(const DRW_Ray& data) override
    {
        // Rendered as an xline for now (honest approximation, counted).
        place(std::make_unique<XLineEntity>(Vec2d{data.basePoint.x, data.basePoint.y},
                                            Vec2d{data.secPoint.x, data.secPoint.y}),
              data);
    }

    void addCircle(const DRW_Circle& data) override
    {
        place(std::make_unique<CircleEntity>(Vec2d{data.basePoint.x, data.basePoint.y},
                                             data.radious),
              data);
    }

    void addArc(const DRW_Arc& data) override
    {
        // DXF arcs: CCW from staangle to endangle (already radians here).
        const double sweep = ccwSweep(data.staangle, data.endangle);
        place(std::make_unique<ArcEntity>(Vec2d{data.basePoint.x, data.basePoint.y},
                                          data.radious, normalizeAngle(data.staangle), sweep),
              data);
    }

    void addEllipse(const DRW_Ellipse& data) override
    {
        double sta = data.staparam;
        double end = data.endparam;
        if (nearEqual(sta, end) || (nearZero(sta) && nearEqual(end, 2.0 * M_PI)))
            sta = 0, end = 2.0 * M_PI;
        else if (end < sta)
            end += 2.0 * M_PI;
        place(std::make_unique<EllipseEntity>(
                  Vec2d{data.basePoint.x, data.basePoint.y},
                  Vec2d{data.secPoint.x, data.secPoint.y}, data.ratio, sta, end),
              data);
    }

    void addLWPolyline(const DRW_LWPolyline& data) override
    {
        std::vector<PolyVertex> verts;
        verts.reserve(data.vertlist.size());
        for (const auto& v : data.vertlist)
            verts.push_back({{v->x, v->y}, v->bulge});
        if (verts.size() < 2) {
            skip("degenerate-lwpolyline");
            return;
        }
        place(std::make_unique<PolylineEntity>(std::move(verts), (data.flags & 1) != 0), data);
    }

    void addPolyline(const DRW_Polyline& data) override
    {
        // Legacy 2D POLYLINE; 3D meshes are skipped.
        if (data.flags & (8 | 16 | 64)) {
            skip("3d-polyline/mesh");
            return;
        }
        std::vector<PolyVertex> verts;
        verts.reserve(data.vertlist.size());
        for (const auto& v : data.vertlist)
            verts.push_back({{v->basePoint.x, v->basePoint.y}, v->bulge});
        if (verts.size() < 2) {
            skip("degenerate-polyline");
            return;
        }
        place(std::make_unique<PolylineEntity>(std::move(verts), (data.flags & 1) != 0), data);
    }

    void addSpline(const DRW_Spline* data) override
    {
        auto s = std::make_unique<SplineEntity>();
        s->degree = data->degree;
        s->closed = (data->flags & 1) != 0;
        for (const auto& c : data->controllist)
            s->controlPoints.push_back({c->x, c->y});
        for (const double k : data->knotslist)
            s->knots.push_back(k);
        // Weights only if meaningful (all-1 lists are common noise).
        bool nontrivial = false;
        for (const double w : data->weightlist)
            nontrivial = nontrivial || !nearEqual(w, 1.0);
        if (nontrivial && data->weightlist.size() == data->controllist.size())
            for (const double w : data->weightlist)
                s->weights.push_back(w);
        for (const auto& f : data->fitlist)
            s->fitPoints.push_back({f->x, f->y});
        if (s->controlPoints.empty() && s->fitPoints.empty()) {
            skip("empty-spline");
            return;
        }
        place(std::move(s), *data);
    }

    // ---- unsupported (counted, will land in later milestones) ---------------

    void addInsert(const DRW_Insert&) override { skip("insert"); }
    void addText(const DRW_Text&) override { skip("text"); }
    void addMText(const DRW_MText&) override { skip("mtext"); }
    void addHatch(const DRW_Hatch*) override { skip("hatch"); }
    void addDimAlign(const DRW_DimAligned*) override { skip("dimension"); }
    void addDimLinear(const DRW_DimLinear*) override { skip("dimension"); }
    void addDimRadial(const DRW_DimRadial*) override { skip("dimension"); }
    void addDimDiametric(const DRW_DimDiametric*) override { skip("dimension"); }
    void addDimAngular(const DRW_DimAngular*) override { skip("dimension"); }
    void addDimAngular3P(const DRW_DimAngular3p*) override { skip("dimension"); }
    void addDimOrdinate(const DRW_DimOrdinate*) override { skip("dimension"); }
    void addLeader(const DRW_Leader*) override { skip("leader"); }
    void addTrace(const DRW_Trace&) override { skip("trace"); }
    void add3dFace(const DRW_3Dface&) override { skip("3dface"); }
    void addSolid(const DRW_Solid&) override { skip("solid-fill"); }
    void addViewport(const DRW_Viewport&) override { skip("viewport"); }
    void addImage(const DRW_Image*) override { skip("image"); }

    // ---- blocks: definitions dropped for now ---------------------------------

    void addBlock(const DRW_Block&) override { inBlock = true; }
    void setBlock(const int) override {}
    void endBlock() override { inBlock = false; }

    // ---- uninteresting callbacks ---------------------------------------------

    void addHeader(const DRW_Header* data) override
    {
        // $INSUNITS: 1 = inches, 4 = mm (default mm).
        auto it = data->vars.find("$INSUNITS");
        if (it != data->vars.end() && it->second->type() == DRW_Variant::INTEGER) {
            const int units = it->second->content.i;
            doc->setDisplayUnits(units == 1 ? DisplayUnits::Inches
                                            : DisplayUnits::Millimeters);
        }
    }
    void addLType(const DRW_LType&) override {}
    void addDimStyle(const DRW_Dimstyle&) override {}
    void addVport(const DRW_Vport&) override {}
    void addTextStyle(const DRW_Textstyle&) override {}
    void addAppId(const DRW_AppId&) override {}
    void addKnot(const DRW_Entity&) override {}
    void linkImage(const DRW_ImageDef*) override {}
    void addComment(const char*) override {}
    void addPlotSettings(const DRW_PlotSettings*) override {}

    // Writer side — never called on import.
    void writeHeader(DRW_Header&) override {}
    void writeBlocks() override {}
    void writeBlockRecords() override {}
    void writeEntities() override {}
    void writeLTypes() override {}
    void writeLayers() override {}
    void writeTextstyles() override {}
    void writeVports() override {}
    void writeDimstyles() override {}
    void writeObjects() override {}
    void writeAppId() override {}
};

} // namespace

DxfImportResult importDxf(const QString& path)
{
    DxfImportResult result;
    Builder builder;
    dxfRW reader(path.toUtf8().constData());
    if (!reader.read(&builder, /*ext=*/false)) {
        result.error = QStringLiteral("failed to parse DXF: %1").arg(path);
        return result;
    }
    result.ok = true;
    result.imported = builder.imported;
    result.skipped = builder.skipped;
    result.skippedTypes = builder.skippedTypes;
    result.document = std::move(builder.doc);
    return result;
}

} // namespace viki
