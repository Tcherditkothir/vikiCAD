#pragma once

#include <vector>

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <gp_Quaternion.hxx>
#include <gp_Vec.hxx>

#include "anim/Chain.h"

namespace viki {
namespace anim {

// Animation of a kinematic chain, read from a GenMov3D `pose3d` v1 JSON file.
//
// The schema stores SPARSE keyframes: a channel missing from a keyframe is
// carried over from the previous keyframe (rest pose before the first) — the
// Moment_MYP v0 convention. We densify at parse time: every DenseKey below is
// a full pose snapshot, so sampling only ever interpolates between two
// complete snapshots (slerp for rotations, linear for scalars). This is NOT
// the same as sparse per-channel tracks: a channel touched at t=0 and t=2 but
// not at t=1 holds its t=0 value until t=1, then moves.
//
// Units: mm / radians / seconds internally (converted at parse, like Chain).

// One joint's value inside a dense keyframe. Which field is meaningful
// depends on the joint type: `rot` for ball, `scalar` for revolute (rad) and
// prismatic (mm). Fixed and free joints ignore both (the root is driven by
// the root channels).
struct JointChannel {
    gp_Quaternion rot;   // identity at rest
    double scalar = 0.0; // 0 at rest
};

struct DenseKey {
    double t = 0.0; // seconds
    gp_Vec rootPosMm{0, 0, 0};
    gp_Quaternion rootRot;
    std::vector<JointChannel> values; // one per chain joint, same indices
};

// A full pose at an arbitrary time (same layout as DenseKey minus the time).
struct PoseSample {
    gp_Vec rootPosMm{0, 0, 0};
    gp_Quaternion rootRot;
    std::vector<JointChannel> values;
};

enum class LoopMode {
    PingPong, // forward then backward over [start, end], loops forever
    Cycle,    // [start, end) then jumps back to start, loops forever
    Hold,     // plays [0, end] once and freezes on the last frame
};

struct AnimClip {
    QString id;
    QString chainId; // must match the chain the clip animates
    QString nameFr, nameEn, source;
    int fps = 24;
    std::vector<DenseKey> keys; // strictly increasing t, at least one

    LoopMode loop = LoopMode::Hold;
    double loopStart = 0.0; // seconds
    double loopEnd = 0.0;   // seconds (defaults to the last keyframe time)

    // Optional micro-oscillation ("breath"): a sine of `breathAmplitudeRad`
    // with period `breathPeriodS`, added to the listed ball joints as an
    // extra local X rotation (revolute: added to the angle). Ignorable by
    // design — pass applyBreath=false to sampleAt.
    bool hasBreath = false;
    double breathPeriodS = 4.0;
    double breathAmplitudeRad = 0.0;
    std::vector<int> breathJoints; // indices into Chain::joints

    double duration() const { return keys.empty() ? 0.0 : keys.back().t; }

    // Full pose at time t (clamped to [first, last] keyframe). Rotations
    // slerp, scalars and root position interpolate linearly.
    PoseSample sampleAt(double t, bool applyBreath = true) const;

    // The exact frame instants (seconds) to render for the WebP loop,
    // according to `loop` and `fps`. PingPong bakes the way back so the
    // encoder can loop the file seamlessly; Cycle omits the end frame
    // (identical to the start by construction); Hold covers [0, end].
    std::vector<double> frameTimes() const;
};

struct ClipResult {
    bool ok = false;
    QString error;
    QStringList warnings; // e.g. joint stops exceeded — advisory only
    AnimClip clip;
};

// Parse a pose3d v1 JSON object against `chain` (channel names and arities
// are validated against the joint types; values beyond the declared stops
// produce warnings, never clamping — the renderer reports faithfully).
ClipResult clipFromJson(const QJsonObject& obj, const Chain& chain);

// Read + parse a pose3d v1 file.
ClipResult loadClipFile(const QString& path, const Chain& chain);

} // namespace anim
} // namespace viki
