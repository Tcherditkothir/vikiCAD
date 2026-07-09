#include "SolidEntity.h"

#include <sstream>

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BinTools.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Dir.hxx>
#include <gp_Trsf.hxx>

#include "render/DrawingProjection.h"

namespace viki {

void SolidEntity::setShape(const TopoDS_Shape& shape)
{
    m_shape = shape;
    updateCache();
}

void SolidEntity::updateCache()
{
    m_bounds2d = BBox2d{};
    m_zmin = m_zmax = 0;
    m_silhouette.clear();
    if (m_shape.IsNull())
        return;
    Bnd_Box box;
    BRepBndLib::Add(m_shape, box);
    if (box.IsVoid())
        return;
    double xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    m_bounds2d = BBox2d{{xmin, ymin}, {xmax, ymax}};
    m_zmin = zmin;
    m_zmax = zmax;

    // Real 2D-canvas representation: the top-view HLR silhouette. Looking along
    // +Z maps the projection's 2D frame 1:1 onto world XY (right=+X, up=+Y), so
    // the outline overlays the solid's true footprint (no mirror). Cached so
    // repaints stay cheap. Guarded by edge count so a huge imported assembly
    // falls back to the fast bounding box instead of a slow HLR pass.
    int edges = 0;
    for (TopExp_Explorer e(m_shape, TopAbs_EDGE); e.More(); e.Next())
        if (++edges > 4000)
            break;
    if (edges > 0 && edges <= 4000) {
        const auto proj = render::projectToDrawing(m_shape, gp_Dir(0, 0, 1), 0.1);
        m_silhouette.reserve(proj.visible.size());
        for (const auto& s : proj.visible)
            m_silhouette.emplace_back(s.a, s.b);
    }
}

BBox2d SolidEntity::bounds() const
{
    return m_bounds2d;
}

void SolidEntity::transform(const Xform2d& xf)
{
    if (m_shape.IsNull())
        return;
    // 2D conformal transforms lift to 3D (rotation about Z + translation +
    // uniform scale). Mirrors map to a scale of -1 about the axis.
    gp_Trsf t;
    const double s = xf.uniformScale();
    t.SetValues(xf.a, xf.c, 0, xf.tx,
                xf.b, xf.d, 0, xf.ty,
                0, 0, (xf.det() >= 0 ? s : -s), 0);
    BRepBuilderAPI_Transform op(m_shape, t, /*copy=*/true);
    if (op.IsDone())
        setShape(op.Shape());
}

void SolidEntity::buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const
{
    // 2D canvas representation: the real top-view silhouette of the solid (the
    // OCCT 3D view remains the primary display). No more generic "[3D WxHxD]"
    // placeholder box.
    if (!m_bounds2d.isValid())
        return;
    if (!m_silhouette.empty()) {
        for (const auto& seg : m_silhouette) {
            StrokePrimitive s;
            s.rgb = ctx.resolvedColor;
            s.points = {seg.first, seg.second};
            out.strokes.push_back(std::move(s));
        }
        return;
    }

    // Fallback (HLR unavailable or shape too complex): the plain XY footprint
    // box — still no label.
    StrokePrimitive box;
    box.rgb = ctx.resolvedColor;
    box.closed = true;
    box.points = {m_bounds2d.min,
                  {m_bounds2d.max.x, m_bounds2d.min.y},
                  m_bounds2d.max,
                  {m_bounds2d.min.x, m_bounds2d.max.y}};
    out.strokes.push_back(std::move(box));
}

void SolidEntity::snapPoints(std::vector<SnapPoint>& out) const
{
    if (!m_bounds2d.isValid())
        return;
    out.push_back({m_bounds2d.min, SnapKind::Endpoint});
    out.push_back({m_bounds2d.max, SnapKind::Endpoint});
    out.push_back({m_bounds2d.center(), SnapKind::Center});
}

QByteArray SolidEntity::shapeToBytes(const TopoDS_Shape& shape)
{
    std::ostringstream stream;
    BinTools::Write(shape, stream);
    const std::string data = stream.str();
    return QByteArray(data.data(), int(data.size()));
}

TopoDS_Shape SolidEntity::shapeFromBytes(const QByteArray& bytes)
{
    TopoDS_Shape shape;
    std::istringstream stream(std::string(bytes.constData(), size_t(bytes.size())));
    BinTools::Read(shape, stream);
    return shape;
}

void SolidEntity::applyTrsf(const gp_Trsf& t)
{
    if (m_shape.IsNull())
        return;
    BRepBuilderAPI_Transform op(m_shape, t, /*copy=*/true);
    if (op.IsDone())
        setShape(op.Shape());
}

void SolidEntity::geomToJson(QJsonObject& obj) const
{
    // BREP as base64: the same serializer feeds the undo journal and the
    // native file. Compact enough at this document scale (zstd deferred).
    obj[QStringLiteral("brep")] =
        QString::fromLatin1(shapeToBytes(m_shape).toBase64());
    // Always emitted so the properties panel shows editable rows even at the
    // default values (empty component / 0 transparency).
    obj[QStringLiteral("component")] = component;
    obj[QStringLiteral("transparency")] = transparency;
}

void SolidEntity::geomFromJson(const QJsonObject& obj)
{
    const QByteArray bytes =
        QByteArray::fromBase64(obj[QStringLiteral("brep")].toString().toLatin1());
    if (!bytes.isEmpty())
        setShape(shapeFromBytes(bytes));
    component = obj[QStringLiteral("component")].toString();
    transparency = obj[QStringLiteral("transparency")].toDouble(0.0);
}

} // namespace viki
