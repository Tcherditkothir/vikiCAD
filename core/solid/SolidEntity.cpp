#include "SolidEntity.h"

#include <sstream>

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BinTools.hxx>
#include <Bnd_Box.hxx>
#include <gp_Trsf.hxx>

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
    // 2D canvas representation: the XY footprint box + a "3D" label.
    // The real display is the OCCT 3D view.
    if (!m_bounds2d.isValid())
        return;
    StrokePrimitive box;
    box.rgb = ctx.resolvedColor;
    box.closed = true;
    box.points = {m_bounds2d.min,
                  {m_bounds2d.max.x, m_bounds2d.min.y},
                  m_bounds2d.max,
                  {m_bounds2d.min.x, m_bounds2d.max.y}};
    out.strokes.push_back(std::move(box));

    TextPrimitive label;
    label.pos = m_bounds2d.center();
    label.height = std::min(5.0, m_bounds2d.height() * 0.2 + 1.0);
    label.text = QStringLiteral("[3D %1x%2x%3]")
                     .arg(m_bounds2d.width(), 0, 'f', 0)
                     .arg(m_bounds2d.height(), 0, 'f', 0)
                     .arg(m_zmax - m_zmin, 0, 'f', 0);
    label.rgb = ctx.resolvedColor;
    label.hAlign = TextHAlign::Center;
    out.texts.push_back(std::move(label));
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

void SolidEntity::geomToJson(QJsonObject& obj) const
{
    // BREP as base64: the same serializer feeds the undo journal and the
    // native file. Compact enough at this document scale (zstd deferred).
    obj[QStringLiteral("brep")] =
        QString::fromLatin1(shapeToBytes(m_shape).toBase64());
}

void SolidEntity::geomFromJson(const QJsonObject& obj)
{
    const QByteArray bytes =
        QByteArray::fromBase64(obj[QStringLiteral("brep")].toString().toLatin1());
    if (!bytes.isEmpty())
        setShape(shapeFromBytes(bytes));
}

} // namespace viki
