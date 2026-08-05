#pragma once

#include <vector>

#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include "anim/AnimClip.h"
#include "anim/Chain.h"

namespace viki {
namespace anim {

// Forward kinematics: world transform of every joint frame for a given pose.
//
// A joint's local transform is translate(attach) ∘ rotate(channel) — for a
// prismatic joint the translation slides along the axis instead, for the
// root the translation/rotation come from the root channels (root_pos is
// ABSOLUTE, replacing the root's rest attach). World transforms compose down
// the chain exactly like the STEP assembly locations do in StepIo
// (parent ∘ local), which is why the joints must be topologically ordered —
// Chain guarantees the root at index 0 and parents before children.
//
// The returned vector is indexed like Chain::joints, in millimetres.
std::vector<gp_Trsf> worldTransforms(const Chain& chain,
                                     const PoseSample& pose);

// Segment endpoints (mm, world) of joint `index` under `transforms`: the
// joint origin and the far end of its segment (restDirection × length in the
// joint's local frame). Meaningful when the joint has a length; for a point
// joint both ends coincide.
gp_Pnt jointOrigin(const std::vector<gp_Trsf>& transforms, int index);
gp_Pnt segmentEnd(const Chain& chain,
                  const std::vector<gp_Trsf>& transforms, int index);

} // namespace anim
} // namespace viki
