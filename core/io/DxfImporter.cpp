#include "DxfImporter.h"

#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <drw_interface.h>
#include <libdwgr.h>
#include <libdxfrw.h>

#include <QJsonDocument>

#include "doc/Annotations.h"
#include "doc/Block.h"
#include "doc/StickyNote.h"
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
    BlockDef* currentBlock = nullptr;

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
        // Layer/color resolution applies to block content too — otherwise
        // everything inside a block lands on layer 0 in white.
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
        if (inBlock) {
            if (currentBlock)
                currentBlock->entities.push_back(std::move(e));
            return;
        }
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
        // A point carrying VIKI_STICKYNOTE XDATA is one of our notes.
        bool isNote = false;
        QStringList strings;
        for (const auto& v : data.extData) {
            if (v->code() == 1001 && v->type() == DRW_Variant::STRING)
                isNote = isNote ||
                         *v->content.s == std::string("VIKI_STICKYNOTE");
            else if (v->code() == 1000 && v->type() == DRW_Variant::STRING)
                strings.append(QString::fromStdString(*v->content.s));
        }
        if (isNote && !strings.isEmpty()) {
            auto note = std::make_unique<StickyNoteEntity>();
            const QJsonObject header =
                QJsonDocument::fromJson(strings.first().toUtf8()).object();
            note->author = header[QStringLiteral("author")].toString();
            note->created = header[QStringLiteral("created")].toString();
            note->modified = header[QStringLiteral("modified")].toString();
            QString text;
            for (int i = 1; i < strings.size(); ++i)
                text += strings[i];
            note->text = text;
            note->anchor = {data.basePoint.x, data.basePoint.y};
            place(std::move(note), data);
            return;
        }
        place(std::make_unique<PointEntity>(Vec2d{data.basePoint.x, data.basePoint.y}), data);
    }

    void addLine(const DRW_Line& data) override
    {
        static int dbgCount = 0;
        if (qEnvironmentVariableIsSet("VIKI_IMPORT_DEBUG") && dbgCount++ < 4)
            fprintf(stderr, "[addLine] inBlock=%d\n", int(inBlock));
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

    // ---- annotations ---------------------------------------------------------

    static QString cleanMtext(QString t)
    {
        t.replace(QLatin1String("\\P"), QLatin1String("\n"));
        return t;
    }

    void addText(const DRW_Text& data) override
    {
        auto t = std::make_unique<TextEntity>(
            Vec2d{data.basePoint.x, data.basePoint.y}, data.height,
            data.angle * M_PI / 180.0, QString::fromStdString(data.text));
        place(std::move(t), data);
    }

    void addMText(const DRW_MText& data) override
    {
        double rotation = 0.0;
        const Vec2d dirVec{data.secPoint.x, data.secPoint.y};
        if (dirVec.lengthSq() > 1e-12) {
            // Code 11: X-axis direction vector — the reliable source.
            rotation = dirVec.angle();
        } else {
            // Code 50: the spec says radians but plenty of producers write
            // degrees. |v| > 2*pi cannot be a sane radian rotation.
            rotation = std::fabs(data.angle) > 2.0 * M_PI + 0.01
                           ? data.angle * M_PI / 180.0
                           : data.angle;
        }
        auto t = std::make_unique<TextEntity>(
            Vec2d{data.basePoint.x, data.basePoint.y}, data.height, rotation,
            cleanMtext(QString::fromStdString(data.text)));
        place(std::move(t), data);
    }

    void addHatch(const DRW_Hatch* data) override
    {
        auto hatch = std::make_unique<HatchEntity>();
        hatch->pattern = data->solid
                             ? QStringLiteral("SOLID")
                             : QString::fromStdString(data->name).toUpper();
        hatch->scale = data->scale > 0 ? data->scale : 1.0;
        hatch->angle = data->angle * M_PI / 180.0;
        for (const auto& loop : data->looplist) {
            std::vector<Vec2d> ring;
            std::vector<std::vector<Vec2d>> edges; // unchained boundary pieces
            for (const auto& obj : loop->objlist) {
                if (const auto* pl =
                        dynamic_cast<const DRW_LWPolyline*>(obj.get())) {
                    const size_t n = pl->vertlist.size();
                    for (size_t i = 0; i < n; ++i) {
                        const auto& v = pl->vertlist[i];
                        if (!nearZero(v->bulge) && n > 1) {
                            const auto& w = pl->vertlist[(i + 1) % n];
                            std::vector<Vec2d> pts;
                            const double sweep = 4.0 * std::atan(v->bulge);
                            const Vec2d va{v->x, v->y}, vb{w->x, w->y};
                            const double chord = va.distanceTo(vb);
                            if (chord > kGeomTol) {
                                const double radius =
                                    chord / (2.0 * std::fabs(std::sin(sweep / 2.0)));
                                const Vec2d mid = (va + vb) * 0.5;
                                const double h = radius * std::cos(sweep / 2.0) *
                                                 (v->bulge > 0 ? 1.0 : -1.0);
                                const Vec2d center =
                                    mid + (vb - va).normalized().perp() * h;
                                flattenArc(center, radius, (va - center).angle(), sweep,
                                           0.05, pts);
                                pts.pop_back();
                                ring.insert(ring.end(), pts.begin(), pts.end());
                                continue;
                            }
                        }
                        ring.push_back({v->x, v->y});
                    }
                } else if (const auto* ln = dynamic_cast<const DRW_Line*>(obj.get())) {
                    edges.push_back({{ln->basePoint.x, ln->basePoint.y},
                                     {ln->secPoint.x, ln->secPoint.y}});
                } else if (const auto* arc = dynamic_cast<const DRW_Arc*>(obj.get())) {
                    std::vector<Vec2d> pts;
                    double sweep = ccwSweep(arc->staangle, arc->endangle);
                    double start = arc->staangle;
                    if (!arc->isccw) { // clockwise edge: reverse traversal
                        start = arc->endangle;
                    }
                    flattenArc({arc->basePoint.x, arc->basePoint.y}, arc->radious,
                               start, sweep, 0.05, pts);
                    if (!arc->isccw)
                        std::reverse(pts.begin(), pts.end());
                    if (pts.size() >= 2)
                        edges.push_back(std::move(pts));
                }
            }
            // Edge loops arrive unordered/reversed in the wild: chain them
            // by endpoint proximity instead of trusting file order.
            if (!edges.empty()) {
                std::vector<Vec2d> chained;
                std::vector<bool> used(edges.size(), false);
                chained.insert(chained.end(), edges[0].begin(), edges[0].end());
                used[0] = true;
                bool grew = true;
                const double tol = 1e-6;
                while (grew) {
                    grew = false;
                    for (size_t ei = 0; ei < edges.size(); ++ei) {
                        if (used[ei])
                            continue;
                        auto piece = edges[ei];
                        if (nearEqual(chained.back(), piece.front(), tol)) {
                            chained.insert(chained.end(), piece.begin() + 1, piece.end());
                        } else if (nearEqual(chained.back(), piece.back(), tol)) {
                            std::reverse(piece.begin(), piece.end());
                            chained.insert(chained.end(), piece.begin() + 1, piece.end());
                        } else {
                            continue;
                        }
                        used[ei] = true;
                        grew = true;
                    }
                }
                if (chained.size() >= 2 && nearEqual(chained.front(), chained.back(), tol))
                    chained.pop_back();
                ring = std::move(chained);
            }
            if (ring.size() >= 3)
                hatch->rings.push_back(std::move(ring));
        }
        if (hatch->rings.empty()) {
            skip("empty-hatch");
            return;
        }
        place(std::move(hatch), *data);
    }

    void addDimLinear(const DRW_DimLinear* data) override
    {
        auto dim = std::make_unique<DimensionEntity>();
        dim->kind = DimensionEntity::Kind::Linear;
        dim->a = {data->getDef1Point().x, data->getDef1Point().y};
        dim->b = {data->getDef2Point().x, data->getDef2Point().y};
        dim->pos = {data->getDimPoint().x, data->getDimPoint().y};
        const double ang = data->getAngle() * M_PI / 180.0;
        dim->axis = Vec2d::polar(1.0, ang);
        place(std::move(dim), *data);
    }

    void addDimAlign(const DRW_DimAligned* data) override
    {
        auto dim = std::make_unique<DimensionEntity>();
        dim->kind = DimensionEntity::Kind::Aligned;
        dim->a = {data->getDef1Point().x, data->getDef1Point().y};
        dim->b = {data->getDef2Point().x, data->getDef2Point().y};
        dim->pos = {data->getDimPoint().x, data->getDimPoint().y};
        place(std::move(dim), *data);
    }

    void addDimRadial(const DRW_DimRadial* data) override
    {
        auto dim = std::make_unique<DimensionEntity>();
        dim->kind = DimensionEntity::Kind::Radius;
        dim->a = {data->getCenterPoint().x, data->getCenterPoint().y};
        dim->b = {data->getDiameterPoint().x, data->getDiameterPoint().y};
        dim->pos = {data->getTextPoint().x, data->getTextPoint().y};
        place(std::move(dim), *data);
    }

    void addDimDiametric(const DRW_DimDiametric* data) override
    {
        auto dim = std::make_unique<DimensionEntity>();
        dim->kind = DimensionEntity::Kind::Diameter;
        const Vec2d p1{data->getDiameter1Point().x, data->getDiameter1Point().y};
        const Vec2d p2{data->getDiameter2Point().x, data->getDiameter2Point().y};
        dim->a = (p1 + p2) * 0.5;
        dim->b = p1;
        dim->pos = {data->getTextPoint().x, data->getTextPoint().y};
        place(std::move(dim), *data);
    }

    void addDimAngular3P(const DRW_DimAngular3p* data) override
    {
        auto dim = std::make_unique<DimensionEntity>();
        dim->kind = DimensionEntity::Kind::Angular;
        dim->a = {data->getVertexPoint().x, data->getVertexPoint().y};
        dim->b = {data->getFirstLine().x, data->getFirstLine().y};
        dim->c = {data->getSecondLine().x, data->getSecondLine().y};
        dim->pos = {data->getDimPoint().x, data->getDimPoint().y};
        place(std::move(dim), *data);
    }

    void addLeader(const DRW_Leader* data) override
    {
        auto leader = std::make_unique<LeaderEntity>();
        for (const auto& v : data->vertexlist)
            leader->points.push_back({v->x, v->y});
        if (leader->points.size() < 2) {
            skip("degenerate-leader");
            return;
        }
        place(std::move(leader), *data);
    }

    // ---- unsupported (counted, will land in later milestones) ---------------

    void addInsert(const DRW_Insert& data) override
    {
        static int dbgCount = 0;
        if (qEnvironmentVariableIsSet("VIKI_IMPORT_DEBUG") && dbgCount++ < 8)
            fprintf(stderr, "[addInsert] block=%s inBlock=%d\n", data.name.c_str(),
                    int(inBlock));
        auto ins = std::make_unique<InsertEntity>();
        ins->blockName = QString::fromStdString(data.name);
        ins->position = {data.basePoint.x, data.basePoint.y};
        // libdxfrw normalizes INSERT rotation to radians at parse time.
        ins->rotation = data.angle;
        ins->scale = data.xscale;
        if (!nearEqual(data.yscale, data.xscale))
            ins->scaleY = data.yscale;
        place(std::move(ins), data);
    }
    void addDimAngular(const DRW_DimAngular*) override { skip("dimension-2line"); }
    void addDimOrdinate(const DRW_DimOrdinate*) override { skip("dimension-ordinate"); }
    void addTrace(const DRW_Trace&) override { skip("trace"); }
    void add3dFace(const DRW_3Dface&) override { skip("3dface"); }
    void addSolid(const DRW_Solid&) override { skip("solid-fill"); }
    void addViewport(const DRW_Viewport&) override { skip("viewport"); }
    void addImage(const DRW_Image*) override { skip("image"); }

    // ---- blocks: definitions dropped for now ---------------------------------

    void addBlock(const DRW_Block& data) override
    {
        if (qEnvironmentVariableIsSet("VIKI_IMPORT_DEBUG"))
            fprintf(stderr, "[addBlock] %s (inBlock was %d)\n", data.name.c_str(),
                    int(inBlock));
        inBlock = true;
        const QString name = QString::fromStdString(data.name);
        // Skip anonymous/system blocks (*Model_Space, *Paper_Space, *D...).
        if (name.startsWith(QLatin1Char('*'))) {
            currentBlock = nullptr;
            return;
        }
        currentBlock =
            doc->createBlock(name, {data.basePoint.x, data.basePoint.y});
    }
    void setBlock(const int) override {}
    void endBlock() override
    {
        if (qEnvironmentVariableIsSet("VIKI_IMPORT_DEBUG"))
            fprintf(stderr, "[endBlock]\n");
        inBlock = false;
        currentBlock = nullptr;
    }

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

namespace {

DxfImportResult finish(Builder& builder)
{
    DxfImportResult result;
    result.ok = true;
    result.imported = builder.imported;
    result.skipped = builder.skipped;
    result.skippedTypes = builder.skippedTypes;
    result.document = std::move(builder.doc);
    return result;
}

} // namespace

DxfImportResult importDxf(const QString& path)
{
    Builder builder;
    dxfRW reader(path.toUtf8().constData());
    if (!reader.read(&builder, /*ext=*/true)) {
        DxfImportResult result;
        result.error = QStringLiteral("failed to parse DXF: %1").arg(path);
        return result;
    }
    return finish(builder);
}

namespace {

// AC1032 (2018+) DWGs are beyond libdxfrw's reader. Fall back to GNU
// LibreDWG's dwg2dxf converter when it is installed (PATH or ~/.local/bin).
DxfImportResult importDwgViaLibredwg(const QString& path, const QString& reason)
{
    DxfImportResult result;
    QString tool = QStandardPaths::findExecutable(QStringLiteral("dwg2dxf"));
    if (tool.isEmpty())
        tool = QStandardPaths::findExecutable(
            QStringLiteral("dwg2dxf"),
            {QDir::homePath() + QStringLiteral("/.local/bin")});
    if (tool.isEmpty()) {
        result.error = reason +
                       QStringLiteral(" — and the dwg2dxf fallback converter "
                                      "(GNU LibreDWG) is not installed");
        return result;
    }
    QTemporaryDir tmp;
    const QString dxfPath = tmp.filePath(QStringLiteral("converted.dxf"));
    QProcess proc;
    proc.start(tool, {QStringLiteral("-y"), QStringLiteral("-o"), dxfPath, path});
    if (!proc.waitForFinished(120000) || proc.exitCode() != 0 ||
        !QFile::exists(dxfPath)) {
        result.error =
            QStringLiteral("dwg2dxf conversion failed: %1")
                .arg(QString::fromLocal8Bit(proc.readAllStandardError()).left(300));
        return result;
    }
    DxfImportResult converted = importDxf(dxfPath);
    if (converted.ok)
        converted.skippedTypes.append(QStringLiteral("via-dwg2dxf"));
    return converted;
}

} // namespace

DxfImportResult importDwg(const QString& path)
{
    Builder builder;
    dwgR reader(path.toUtf8().constData());
    if (!reader.read(&builder, /*ext=*/true)) {
        const QString reason =
            QStringLiteral("libdxfrw cannot read this DWG (2018+ format or "
                           "damaged) [dwg error %1]")
                .arg(int(reader.getError()));
        return importDwgViaLibredwg(path, reason);
    }
    return finish(builder);
}

} // namespace viki
