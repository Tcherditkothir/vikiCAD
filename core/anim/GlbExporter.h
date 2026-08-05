#pragma once

#include <QString>

#include "anim/AnimClip.h"
#include "anim/Avatar.h"
#include "anim/Chain.h"

namespace viki {
namespace anim {

struct GlbResult {
    bool ok = false;
    QString error;
    int meshes = 0;        // one glTF mesh per avatar part
    int triangles = 0;     // total across all meshes
    int animatedNodes = 0; // joints that actually received a channel
    qint64 bytes = 0;      // size of the written .glb
};

// Animated GLB (binary glTF 2.0) export: the dressed chain as a node
// hierarchy plus ONE animation baked from the clip's dense keyframes.
//
// Layout decisions, in file order:
//  - A single fix-up root node converts our Z-up millimetre world to the
//    glTF convention (Y-up metres, front facing +Z): rotation
//    Ry(180)*Rx(-90) — quaternion (0, √.5, √.5, 0) — plus uniform scale
//    0.001. Everything below it — node translations, animation outputs,
//    mesh vertices — stays in chain millimetres, Z-up.
//  - One node per joint (chain order, node index = joint index + 1);
//    avatar parts hang under their joint as mesh nodes, in the joint's
//    LOCAL frame, so the joint's animated transform carries them.
//  - Rotations animate as quaternion samplers (LINEAR — spec slerp),
//    prismatic joints and the root as translation samplers. A typed root
//    (revolute/prismatic/ball) composes with root_pos/root_rot exactly
//    like the FK does. Channels that stay at rest through the whole clip
//    are skipped. Consecutive quaternions are sign-aligned so naive
//    component lerp never spins the long way.
//  - loop.mode pingpong bakes the LOOP WINDOW and its way back (times
//    start at 0 = loop.start) so one playthrough is one seamless loop
//    period — the same content the WebP loops over; hold and cycle export
//    the authored keys. The authored loop block rides in asset "extras"
//    for consumers that want to do better.
//  - Written with Qt JSON + a hand-rolled GLB container on purpose; the
//    INDEPENDENT reader (vendored cgltf) only lives in the tests, per the
//    LESSONS rule that a writer is judged by a foreign re-read.
GlbResult exportGlb(const Chain& chain, const AnimClip& clip,
                    const AvatarProvider& provider,
                    const AvatarSpec& avatar, const QString& path,
                    double deflectionMm = 0.8);

} // namespace anim
} // namespace viki
