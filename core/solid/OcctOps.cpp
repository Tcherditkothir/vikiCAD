#include "OcctOps.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <TopoDS_Shape.hxx>

namespace viki {

bool occtSmokeTest()
{
    BRepPrimAPI_MakeBox box(1.0, 1.0, 1.0);
    box.Build();
    return box.IsDone() && !box.Shape().IsNull();
}

} // namespace viki
