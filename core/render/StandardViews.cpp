#include "render/StandardViews.h"

#include <cmath>

#include <gp_Vec.hxx>

#include "solid/SolidOps.h"

namespace viki {
namespace views {

namespace {

// Build an orientation, snapping `up` to be exactly orthogonal to `dir` via
// Gram-Schmidt so the pair is always a valid camera basis.
ViewOrientation make(const gp_Vec& dir, const gp_Vec& up)
{
    const gp_Dir d(dir);
    gp_Vec u = up - gp_Vec(d) * (up.Dot(gp_Vec(d)));
    // Degenerate up (parallel to dir) — pick any orthogonal fallback.
    if (u.Magnitude() < 1e-9) {
        gp_Vec ref = std::abs(d.Z()) < 0.9 ? gp_Vec(0, 0, 1) : gp_Vec(1, 0, 0);
        u = ref - gp_Vec(d) * (ref.Dot(gp_Vec(d)));
    }
    return ViewOrientation{d, gp_Dir(u)};
}

} // namespace

std::optional<ViewOrientation> standardViewDir(const QString& name)
{
    const QString n = name.trimmed().toUpper();

    if (n == QLatin1String("TOP"))
        return make(gp_Vec(0, 0, -1), gp_Vec(0, 1, 0));
    if (n == QLatin1String("BOTTOM"))
        return make(gp_Vec(0, 0, 1), gp_Vec(0, -1, 0));
    if (n == QLatin1String("FRONT"))
        return make(gp_Vec(0, 1, 0), gp_Vec(0, 0, 1));
    if (n == QLatin1String("BACK"))
        return make(gp_Vec(0, -1, 0), gp_Vec(0, 0, 1));
    if (n == QLatin1String("LEFT"))
        return make(gp_Vec(1, 0, 0), gp_Vec(0, 0, 1));
    if (n == QLatin1String("RIGHT"))
        return make(gp_Vec(-1, 0, 0), gp_Vec(0, 0, 1));
    if (n == QLatin1String("ISO") || n == QLatin1String("ISOMETRIC") ||
        n == QLatin1String("NE-ISO") || n == QLatin1String("NEISO"))
        // Standard NE isometric: eye above +X/+Y, looking down toward origin.
        // Up chosen as world +Z projected onto the view plane so vertical
        // edges read as vertical on screen.
        return make(gp_Vec(1, 1, -1), gp_Vec(0, 0, 1));

    return std::nullopt;
}

std::optional<ViewOrientation> alignToFaceDir(const TopoDS_Shape& face)
{
    const auto wp = solidops::planeFromFace(face);
    if (!wp)
        return std::nullopt;
    // Camera faces the surface: look along the reversed outward normal.
    const gp_Vec look = gp_Vec(wp->normal).Reversed();
    // Prefer world +Z for up; if the face normal is (anti)parallel to Z, fall
    // back to the plane's own xDir so `up` is well-defined for top/bottom faces.
    gp_Vec up(0, 0, 1);
    if (std::abs(gp_Vec(wp->normal).Dot(gp_Vec(0, 0, 1))) > 0.999)
        up = gp_Vec(wp->xDir);
    return make(look, up);
}

} // namespace views
} // namespace viki
