#include "GearGeometry.h"

#include <algorithm>
#include <cmath>

namespace viki {
namespace gear {
namespace {

// Involute function inv(a) = tan(a) - a.
double inv(double a) { return std::tan(a) - a; }

} // namespace

GearMetrics metrics(const GearParams& p)
{
    const double m = p.module;
    const int z = std::max(p.teeth, 1);
    const double alpha = p.pressureAngleDeg * M_PI / 180.0;

    GearMetrics g;
    g.pitchDiameter = m * z;
    g.baseDiameter = m * z * std::cos(alpha);
    g.addendum = p.addendumCoef * m;
    g.dedendum = p.dedendumCoef * m;
    g.outsideDiameter = g.pitchDiameter + 2.0 * g.addendum;
    g.rootDiameter = g.pitchDiameter - 2.0 * g.dedendum;
    g.circularPitch = M_PI * m;
    g.toothThickness = g.circularPitch / 2.0;
    g.wholeDepth = g.addendum + g.dedendum;
    const double sa = std::sin(alpha);
    g.minTeethNoUndercut = 2.0 * p.addendumCoef / (sa * sa);
    g.undercut = double(z) < g.minTeethNoUndercut - 1e-9;
    return g;
}

std::vector<Vec2d> profile(const GearParams& p, const Vec2d& center,
                           double chordTol)
{
    const double m = p.module;
    const int z = std::max(p.teeth, 3);
    const double alpha = p.pressureAngleDeg * M_PI / 180.0;

    const double rp = m * z / 2.0;
    const double rb = rp * std::cos(alpha);
    const double ra = rp + p.addendumCoef * m;
    const double rf = std::max(rp - p.dedendumCoef * m, 0.5 * m);
    const double psi = M_PI / (2.0 * z); // half tooth angle at pitch circle
    const double invAlpha = inv(alpha);

    // Involute polar-angle offset at radius r (>= rb).
    const auto involuteAngle = [&](double r) {
        const double c = std::clamp(rb / r, -1.0, 1.0);
        return inv(std::acos(c));
    };
    // Half tooth angle at radius r (shrinks with r; zero => pointed tip).
    const auto halfAngle = [&](double r) {
        return psi + invAlpha - involuteAngle(std::max(r, rb));
    };

    // Cap the tip if the flanks would cross before reaching the addendum
    // circle (happens for very low tooth counts) -> a pointed tooth.
    double raEff = ra;
    bool pointed = false;
    if (halfAngle(ra) <= 1e-4) {
        pointed = true;
        double lo = std::max(rb, rf), hi = ra;
        for (int it = 0; it < 60; ++it) {
            const double mid = 0.5 * (lo + hi);
            (halfAngle(mid) > 0.0 ? lo : hi) = mid;
        }
        raEff = lo;
    }

    const double rStart = std::max(rb, rf);
    if (chordTol <= 0.0)
        chordTol = 0.05;
    const int nFlank = std::clamp(
        int(std::ceil((raEff - rStart) / std::max(chordTol, 1e-4))), 6, 300);
    const int nTip = 4;
    const int nRoot = 4;

    const auto polar = [&](double r, double ang) {
        return center + Vec2d{r * std::cos(ang), r * std::sin(ang)};
    };
    // Signed flank angle: +1 = left flank, -1 = right flank.
    const auto flankAngle = [&](double r, int side) {
        return double(side) * (psi + invAlpha - involuteAngle(r));
    };

    std::vector<Vec2d> pts;
    pts.reserve(size_t(z) * size_t(2 * nFlank + nTip + nRoot + 4));

    for (int k = 0; k < z; ++k) {
        const double c = 2.0 * M_PI * k / double(z);

        // Right root (radial part below the base circle, if any).
        if (rf < rb)
            pts.push_back(polar(rf, c + flankAngle(rb, -1)));
        // Right flank, root -> tip.
        for (int i = 0; i <= nFlank; ++i) {
            const double r = rStart + (raEff - rStart) * i / nFlank;
            pts.push_back(polar(r, c + flankAngle(r, -1)));
        }
        // Tip arc (skip when pointed: the flanks already meet).
        if (!pointed) {
            const double aR = flankAngle(raEff, -1);
            const double aL = flankAngle(raEff, +1);
            for (int i = 1; i < nTip; ++i)
                pts.push_back(polar(raEff, c + aR + (aL - aR) * i / nTip));
        }
        // Left flank, tip -> root.
        for (int i = 0; i <= nFlank; ++i) {
            const double r = raEff - (raEff - rStart) * i / nFlank;
            pts.push_back(polar(r, c + flankAngle(r, +1)));
        }
        if (rf < rb)
            pts.push_back(polar(rf, c + flankAngle(rb, +1)));

        // Root arc across the gap to the next tooth's right root.
        const double nextC = 2.0 * M_PI * (k + 1) / double(z);
        const double aLroot = (rf < rb) ? flankAngle(rb, +1) : flankAngle(rf, +1);
        const double aRroot = (rf < rb) ? flankAngle(rb, -1) : flankAngle(rf, -1);
        const double a0 = c + aLroot;
        const double a1 = nextC + aRroot;
        for (int i = 1; i < nRoot; ++i)
            pts.push_back(polar(rf, a0 + (a1 - a0) * i / nRoot));
    }
    return pts;
}

} // namespace gear
} // namespace viki
