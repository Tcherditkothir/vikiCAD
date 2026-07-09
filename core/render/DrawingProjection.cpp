#include "DrawingProjection.h"

#include <cmath>

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

namespace viki {
namespace render {

namespace {

// Pick a stable "up" reference not parallel to `d`.
gp_Dir pickUpRef(const gp_Dir& d)
{
    // World +Z unless the view is (nearly) vertical, then use +Y.
    if (std::abs(d.Z()) > 0.9)
        return gp_Dir(0, 1, 0);
    return gp_Dir(0, 0, 1);
}

// Discretize one projected edge into 2D segments. HLRBRep_HLRToShape already
// returns the projected geometry in the projector's own 2D coordinate system:
// X = right, Y = up, Z (depth) ~= 0. So we read X/Y directly.
void addEdge(const TopoDS_Edge& edge, double deflection,
             std::vector<DrawingSegment>& out)
{
    BRepAdaptor_Curve curve(edge);
    std::vector<Vec2d> pts;
    try {
        // TangentialDeflection gives a good chord approximation for any curve;
        // straight lines collapse to their two endpoints.
        GCPnts_TangentialDeflection tessel(curve, 0.2, deflection);
        const int n = tessel.NbPoints();
        if (n < 2)
            return;
        pts.reserve(static_cast<size_t>(n));
        for (int i = 1; i <= n; ++i) {
            const gp_Pnt p = tessel.Value(i);
            pts.push_back(Vec2d{p.X(), p.Y()});
        }
    } catch (const Standard_Failure&) {
        return;
    }
    for (size_t i = 1; i < pts.size(); ++i)
        out.push_back(DrawingSegment{pts[i - 1], pts[i]});
}

void collect(const TopoDS_Shape& compound, double deflection,
             std::vector<DrawingSegment>& out)
{
    if (compound.IsNull())
        return;
    for (TopExp_Explorer exp(compound, TopAbs_EDGE); exp.More(); exp.Next())
        addEdge(TopoDS::Edge(exp.Current()), deflection, out);
}

} // namespace

DrawingProjection projectToDrawing(const TopoDS_Shape& shape, const gp_Dir& viewDir,
                                   double deflection)
{
    DrawingProjection result;
    if (shape.IsNull())
        return result;
    if (deflection <= 0.0)
        deflection = 0.05;

    // Build the projection frame. The Ax2 main direction is the view/projection
    // direction; the projected drawing lives in its XY plane. We want the plane
    // X axis to read as "right" and Y as "up" for a natural drawing.
    const gp_Dir upRef = pickUpRef(viewDir);
    gp_Dir right = gp_Dir(gp_Vec(upRef).Crossed(gp_Vec(viewDir)));
    // gp_Ax2(origin, N, Vx): N is the main (projection) direction, Vx spans X.
    const gp_Ax2 frame(gp_Pnt(0, 0, 0), viewDir, right);

    HLRAlgo_Projector projector(frame);

    Handle(HLRBRep_Algo) hlr = new HLRBRep_Algo();
    try {
        hlr->Add(shape);
        hlr->Projector(projector);
        hlr->Update();
        hlr->Hide();
    } catch (const Standard_Failure&) {
        return result;
    }

    HLRBRep_HLRToShape toShape(hlr);

    // Visible: sharp outline + smooth visible edges. Hidden: the occluded ones.
    TopoDS_Shape vis, visSmooth, hid, hidSmooth;
    try {
        vis = toShape.VCompound();
        visSmooth = toShape.Rg1LineVCompound();
        hid = toShape.HCompound();
        hidSmooth = toShape.Rg1LineHCompound();
    } catch (const Standard_Failure&) {
        return result;
    }

    collect(vis, deflection, result.visible);
    collect(visSmooth, deflection, result.visible);
    collect(hid, deflection, result.hidden);
    collect(hidSmooth, deflection, result.hidden);

    return result;
}

} // namespace render
} // namespace viki
