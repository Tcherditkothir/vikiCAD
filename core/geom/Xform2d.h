#pragma once

#include "Vec2d.h"

namespace viki {

// 2x3 affine transform:  [a c tx] [x]
//                        [b d ty] [y]
struct Xform2d {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;

    static Xform2d translation(const Vec2d& t) { return {1, 0, 0, 1, t.x, t.y}; }
    static Xform2d rotation(double radians, const Vec2d& center = {});
    static Xform2d scaling(double factor, const Vec2d& center = {});

    Vec2d apply(const Vec2d& p) const
    {
        return {a * p.x + c * p.y + tx, b * p.x + d * p.y + ty};
    }
    // Direction vectors ignore translation.
    Vec2d applyVector(const Vec2d& v) const
    {
        return {a * v.x + c * v.y, b * v.x + d * v.y};
    }

    // this ∘ o : apply o first, then this.
    Xform2d compose(const Xform2d& o) const
    {
        return {a * o.a + c * o.b,      b * o.a + d * o.b,
                a * o.c + c * o.d,      b * o.c + d * o.d,
                a * o.tx + c * o.ty + tx, b * o.tx + d * o.ty + ty};
    }

    // True if the transform preserves circles (uniform scale, no shear).
    bool isConformal(double tol = kGeomTol) const
    {
        return nearEqual(a * a + b * b, c * c + d * d, tol) && nearZero(a * c + b * d, tol);
    }
    double uniformScale() const { return Vec2d{a, b}.length(); }
    // z-sign of the determinant: negative means the transform mirrors.
    double det() const { return a * d - b * c; }
};

} // namespace viki
