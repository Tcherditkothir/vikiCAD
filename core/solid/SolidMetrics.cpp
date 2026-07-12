#include "SolidMetrics.h"

#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>

namespace viki {
namespace solidops {

SolidMetrics solidMetrics(const TopoDS_Shape& shape)
{
    SolidMetrics m;
    if (shape.IsNull())
        return m;
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsVoid())
        return m;
    double xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    m.bboxMin = gp_Pnt(xmin, ymin, zmin);
    m.bboxMax = gp_Pnt(xmax, ymax, zmax);

    GProp_GProps vol;
    BRepGProp::VolumeProperties(shape, vol);
    m.volume = vol.Mass();
    m.centroid = vol.CentreOfMass();

    GProp_GProps surf;
    BRepGProp::SurfaceProperties(shape, surf);
    m.area = surf.Mass();

    m.valid = true;
    return m;
}

} // namespace solidops
} // namespace viki
