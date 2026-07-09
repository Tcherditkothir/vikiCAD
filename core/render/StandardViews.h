#pragma once

#include <optional>

#include <QString>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>

namespace viki {

// Camera orientation for a standard/named view: the direction the camera looks
// ALONG (view direction, from eye toward target) plus the up vector. Both are
// unit vectors in world coordinates (mm-agnostic — pure orientation). This is
// the testable core of a ViewCube / "standard views" widget; the widget itself
// (buttons, animation) is GUI and lives elsewhere.
struct ViewOrientation {
    gp_Dir dir; // look direction (eye -> target)
    gp_Dir up;  // camera up vector, orthogonal to dir
};

namespace views {

// Named standard views. Accepts case-insensitive names:
//   TOP, BOTTOM, FRONT, BACK, LEFT, RIGHT, ISO (a.k.a. ISOMETRIC, NE-ISO).
// Returns std::nullopt for an unknown name.
//
// Conventions (Z-up, right-handed world; matches CAD "front = looking north"):
//   TOP    : look -Z, up +Y   (plan view, looking straight down)
//   BOTTOM : look +Z, up -Y
//   FRONT  : look +Y, up +Z    (camera in front, looking toward +Y)
//   BACK   : look -Y, up +Z
//   LEFT   : look +X, up +Z
//   RIGHT  : look -X, up +Z
//   ISO    : look (1,1,-1)/|.|, up chosen so +Z projects upward (NE isometric)
std::optional<ViewOrientation> standardViewDir(const QString& name);

// View orientation that looks straight at a planar face along its (outward)
// normal: the camera faces the surface, so the view direction is the reversed
// normal. `up` is derived deterministically from the normal. Returns
// std::nullopt if the shape is not a planar face.
std::optional<ViewOrientation> alignToFaceDir(const TopoDS_Shape& face);

} // namespace views

} // namespace viki
