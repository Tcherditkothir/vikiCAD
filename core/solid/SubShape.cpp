#include "SubShape.h"

#include <TopAbs_ShapeEnum.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

namespace viki {
namespace subshape {
namespace {

// TopExp::MapShapes fills the map in TopExp_Explorer first-visit order with
// shared sub-shapes registered exactly once — that IS the deterministic
// index (map is 1-based, our API is 0-based).
TopTools_IndexedMapOfShape mapOf(const TopoDS_Shape& shape, TopAbs_ShapeEnum kind)
{
    TopTools_IndexedMapOfShape map;
    if (!shape.IsNull())
        TopExp::MapShapes(shape, kind, map);
    return map;
}

TopoDS_Shape at(const TopoDS_Shape& shape, TopAbs_ShapeEnum kind, int index)
{
    const auto map = mapOf(shape, kind);
    if (index < 0 || index >= map.Extent())
        return {};
    return map.FindKey(index + 1);
}

int indexOf(const TopoDS_Shape& shape, TopAbs_ShapeEnum kind,
            const TopoDS_Shape& sub)
{
    if (sub.IsNull() || sub.ShapeType() != kind)
        return -1;
    // FindIndex matches with IsSame (orientation ignored); 0 = not found.
    return mapOf(shape, kind).FindIndex(sub) - 1;
}

} // namespace

int faceCount(const TopoDS_Shape& shape)
{
    return mapOf(shape, TopAbs_FACE).Extent();
}

int edgeCount(const TopoDS_Shape& shape)
{
    return mapOf(shape, TopAbs_EDGE).Extent();
}

TopoDS_Shape faceAt(const TopoDS_Shape& shape, int index)
{
    return at(shape, TopAbs_FACE, index);
}

TopoDS_Shape edgeAt(const TopoDS_Shape& shape, int index)
{
    return at(shape, TopAbs_EDGE, index);
}

int faceIndexOf(const TopoDS_Shape& shape, const TopoDS_Shape& face)
{
    return indexOf(shape, TopAbs_FACE, face);
}

int edgeIndexOf(const TopoDS_Shape& shape, const TopoDS_Shape& edge)
{
    return indexOf(shape, TopAbs_EDGE, edge);
}

} // namespace subshape
} // namespace viki
