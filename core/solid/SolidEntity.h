#pragma once

#include <utility>
#include <vector>

#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>

#include "doc/Entity.h"
#include "geom/Vec2d.h"

namespace viki {

// 3D solid: a thin wrapper around an OCCT TopoDS_Shape — the one place in
// the 2D document model where OCCT geometry lives. Serialized as a BinTools
// stream (base64 in JSON deltas, raw BLOB potential later).
class SolidEntity : public Entity {
public:
    SolidEntity() = default;
    explicit SolidEntity(const TopoDS_Shape& shape) { setShape(shape); }

    const char* typeName() const override { return "solid"; }
    std::unique_ptr<Entity> clone() const override
    {
        return std::make_unique<SolidEntity>(*this);
    }
    BBox2d bounds() const override;
    void transform(const Xform2d& xf) override;
    void buildPrimitives(const RenderContext& ctx, PrimitiveList& out) const override;
    void snapPoints(std::vector<SnapPoint>& out) const override;

    const TopoDS_Shape& shape() const { return m_shape; }
    void setShape(const TopoDS_Shape& shape);

    // Apply a full 3D placement to the shape (MOVE3D/ROTATE3D, assembly
    // positioning). Unlike transform(Xform2d) this is not limited to XY.
    void applyTrsf(const gp_Trsf& t);

    // Assembly component name (empty = ungrouped). Names a part in the tree.
    QString component;
    // 3D appearance: 0 = opaque, 1 = fully transparent. Color uses the base
    // Entity color (ByLayer or explicit rgb).
    double transparency = 0.0;

    // Serialized BREP (BinTools binary stream).
    static QByteArray shapeToBytes(const TopoDS_Shape& shape);
    static TopoDS_Shape shapeFromBytes(const QByteArray& bytes);

protected:
    void geomToJson(QJsonObject& obj) const override;
    void geomFromJson(const QJsonObject& obj) override;

private:
    void updateCache();

    TopoDS_Shape m_shape;
    BBox2d m_bounds2d;              // XY projection of the 3D box
    double m_zmin = 0, m_zmax = 0;
    // Cached top-view (HLR) silhouette in world XY — the real 2D-canvas
    // representation, recomputed on every shape change. Empty => fall back to
    // the bounding box (HLR failed or the shape was too complex).
    std::vector<std::pair<Vec2d, Vec2d>> m_silhouette;
};

} // namespace viki
