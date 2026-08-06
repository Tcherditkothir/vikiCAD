#pragma once

#include <optional>
#include <vector>

#include <QJsonObject>
#include <QMap>
#include <QString>

#include <gp_Vec.hxx>

namespace viki {
namespace anim {

// Generic kinematic chain, read from a GenMov3D `chain` v1 JSON file. The
// humanoid skeleton is just one chain among others: the same structures drive
// mechanisms, articulated assemblies and animated exploded views.
//
// Conventions (from the schema): right-handed world, Z up, Y+ = front of the
// object, X+ = its right seen from behind. The REST pose is the configuration
// where every joint value is zero; in that pose every joint frame is aligned
// with the world frame, so `restDirection` (given in world coordinates) is
// also the segment direction in the joint's local frame.
//
// Units INSIDE these structs follow the repo rule — millimetres and radians
// everywhere. The schema speaks metres / degrees / fractions of
// `scale_reference`; the conversion happens once, at parse time.

enum class JointType {
    Ball,      // 3 rotations (local Euler XYZ, stored as quaternion)
    Revolute,  // 1 rotation about `axis`
    Prismatic, // 1 translation along `axis`
    Fixed,     // rigid link, no channel
    Free,      // 6 DOF — root only (placed via root_pos / root_rot)
};

struct JointLimit {
    double min = 0.0; // rad for rotations, mm for prismatic
    double max = 0.0;
};

struct Joint {
    QString name;
    int parent = -1; // index into Chain::joints, -1 for the root
    JointType type = JointType::Fixed;
    gp_Vec axis{0, 0, 1};            // unit, local; revolute/prismatic only
    gp_Vec attachMm{0, 0, 0};        // attach point in the PARENT frame (mm)
    gp_Vec restDirection{0, 0, -1};  // unit; meaningful when lengthMm > 0
    double lengthMm = 0.0;           // 0 = point joint (no segment)
    // Per-channel stops, keyed "x"/"y"/"z" (ball) or "x" (revolute/prismatic).
    // Advisory: poses beyond the stops render as-is but produce warnings.
    QMap<QString, JointLimit> limits;
};

struct Chain {
    QString id;
    QString name;
    // accepts_pose_chains extension field: ids of chains whose pose3d
    // files this chain also renders (superset chains — humanoid-14 lists
    // humanoid-12; the extra joints stay at rest, with a warning).
    QStringList acceptsPoseChains;
    double scaleMm = 1000.0; // scale_reference (or avatar override), in mm
    // Topologically ordered at parse time: a joint's parent always has a
    // smaller index. Index 0 is the root.
    std::vector<Joint> joints;

    int indexOf(const QString& jointName) const;
    const Joint& root() const { return joints.front(); }
};

struct ChainResult {
    bool ok = false;
    QString error;
    QStringList warnings;
    Chain chain;
};

// Parse a chain v1 JSON object. `scaleOverrideM` replaces the chain's
// scale_reference (metres) — that is how an avatar's `height_m` resizes the
// chain it dresses. Validates: schema_version, unique joint names, exactly one
// root, resolvable parents, no cycles, axis present for revolute/prismatic,
// rest_direction present whenever length > 0. Joints come out topologically
// sorted with the root at index 0.
ChainResult chainFromJson(const QJsonObject& obj,
                          std::optional<double> scaleOverrideM = {});

// Read + parse a chain v1 file (clear error on unreadable/invalid JSON).
ChainResult loadChainFile(const QString& path,
                          std::optional<double> scaleOverrideM = {});

} // namespace anim
} // namespace viki
