#pragma once

#include <vector>

#include <QString>

#include "solid/FeatureTree.h"

namespace viki {
namespace featureparams {

// ---------------------------------------------------------------------------
// The bridge between a FeatureTree and a properties grid: enumerate the
// scalar parameters a user may edit after placement (Fusion parity), and
// apply one edit through the tree's checked setters. Pure core logic so the
// Properties panel stays a thin widget shim and the behaviour is testable
// headless.
// ---------------------------------------------------------------------------

// One editable scalar parameter of a feature node, grid-ready. `label` is
// "<kind> <nodeIndex>: <param>", e.g. "hole 2: diameter".
struct Param {
    int nodeIndex = -1;
    QString name;  // "height" | "diameter" | "depth" | "thickness"
                   // | "center x" | "center y" (signed — MOVES the hole)
    QString label; // "hole 2: diameter"
    double value = 0.0;
};

// Editable parameters of the tree, in node order. Sketch and BaseShape nodes
// contribute nothing (nothing scalar-editable); a through hole hides its
// ignored depth.
std::vector<Param> list(const FeatureTree& tree);

// Apply one parameter through the tree's kind-checked setters. Returns false
// (tree untouched) on an unknown param name, a node/kind mismatch, or a
// non-positive value — every exposed parameter is a strictly positive length.
// Callers then re-run regenerate()/regenerateFeatures().
bool set(FeatureTree& tree, int nodeIndex, const QString& name, double value);

} // namespace featureparams
} // namespace viki
