#pragma once

#include <cmath>

namespace viki {

// All document geometry is in millimeters, world coordinates.
struct Vec2d {
    double x = 0.0;
    double y = 0.0;

    constexpr Vec2d() = default;
    constexpr Vec2d(double x_, double y_) : x(x_), y(y_) {}

    constexpr Vec2d operator+(const Vec2d& o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2d operator-(const Vec2d& o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2d operator*(double s) const { return {x * s, y * s}; }
    constexpr Vec2d operator/(double s) const { return {x / s, y / s}; }
    constexpr Vec2d& operator+=(const Vec2d& o) { x += o.x; y += o.y; return *this; }
    constexpr Vec2d& operator-=(const Vec2d& o) { x -= o.x; y -= o.y; return *this; }

    constexpr double dot(const Vec2d& o) const { return x * o.x + y * o.y; }
    // z-component of the 3D cross product; sign gives winding.
    constexpr double cross(const Vec2d& o) const { return x * o.y - y * o.x; }

    double length() const { return std::hypot(x, y); }
    constexpr double lengthSq() const { return x * x + y * y; }
    double distanceTo(const Vec2d& o) const { return (o - *this).length(); }
    double angle() const { return std::atan2(y, x); }

    Vec2d normalized() const;
    // Perpendicular, rotated +90 degrees.
    constexpr Vec2d perp() const { return {-y, x}; }
    Vec2d rotated(double radians) const;

    static Vec2d polar(double distance, double radians)
    {
        return {distance * std::cos(radians), distance * std::sin(radians)};
    }
};

constexpr Vec2d operator*(double s, const Vec2d& v) { return v * s; }

// Tolerance for coincidence tests, in mm. Well below drafting precision,
// well above double noise at typical drawing extents.
inline constexpr double kGeomTol = 1e-9;

inline bool nearZero(double v, double tol = kGeomTol) { return std::fabs(v) < tol; }
inline bool nearEqual(double a, double b, double tol = kGeomTol) { return std::fabs(a - b) < tol; }
inline bool nearEqual(const Vec2d& a, const Vec2d& b, double tol = kGeomTol)
{
    return (a - b).lengthSq() < tol * tol;
}

} // namespace viki
