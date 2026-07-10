#pragma once

#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

namespace viki {

// Work plane for sketching/extruding: an orthonormal frame. A 2D sketch point
// (u,v) maps to origin + u*xDir + v*(normal x xDir); extrusion runs along the
// normal. Default = the world XY plane, so 2D drafting is unchanged.
// Lives in its own header so the Document (sketch registry) can store planes
// without pulling in the whole of SolidOps.
struct WorkPlane {
    gp_Pnt origin = gp_Pnt(0, 0, 0);
    gp_Dir normal = gp_Dir(0, 0, 1);
    gp_Dir xDir = gp_Dir(1, 0, 0);
};

} // namespace viki
