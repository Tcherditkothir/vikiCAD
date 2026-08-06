#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include <TopoDS_Shape.hxx>

#include "anim/Chain.h"

namespace viki {
namespace anim {

// Avatar (GenMov3D `avatar` v1): how a kinematic chain LOOKS. The spec is
// pure data; geometry generation goes through the AvatarProvider interface
// so a skinned (deformable-mesh) avatar can plug in later without touching
// the renderer or the GLB exporter — they only ever see per-joint parts.

enum class AvatarType { Rigid, Skinned };
enum class JointStyle { Blend, Sphere, None };
enum class RigidBuild { Capsule, Sculpted };

// Sculpted-build parameters (rigid.sculpt extension block, all fractions of
// scale_reference). These fields are NOT in avatar schema v1: they ride as
// tolerated extra properties and are proposed for v2 in the GenMov3D
// contract (entry SCHEMA-AVATAR-V2-DEMANDE). Absent block = defaults.
struct TorsoSculpt {
    QString joint = QStringLiteral("spine"); // which joint gets the loft
    double hip = 0.058;      // section half-widths, bottom to top. The hem
                             // stays NARROWER than the pelvis blob and
                             // tucks inside it — a wide hem over a narrow
                             // pelvis reads as a skirt (iteration 2 bug)
    double waist = 0.054;
    double chest = 0.066;
    double shoulder = 0.078; // shoulder LINE of the loft; the deltoid
                             // spheres at the arm attach points make the
                             // actual shoulder width
    double depth = 0.62;     // section half-depth = half-width * depth
};

struct PelvisSculpt {
    QString joint = QStringLiteral("pelvis"); // usually the zero-length root
    double width = 0.085;    // ellipsoid half-extents
    double depth = 0.042;
    double drop = 0.060;     // how far the blob rounds below the hip line
};

struct SculptParams {
    // Distal/proximal radius ratio for LEAF segments, "default" plus
    // per-joint overrides (same shape as segment_radius): a wrist tapers, a
    // neck should not. Segments continued by a child ignore this — they
    // taper to the child's radius instead.
    QMap<QString, double> taperFrac;
    double taperFor(const QString& jointName) const;
    double bulge = 1.06; // mid-limb swell factor
    std::optional<TorsoSculpt> torso;
    std::optional<PelvisSculpt> pelvis;
    // Per-joint vertical squash (0..1] of the fusiform solid, e.g. feet and
    // hands. Axis rule: local Z unless the joint's rest_direction is ±Z,
    // then local Y.
    QMap<QString, double> flatten;
};

struct AvatarSpec {
    QString id;
    QString name;
    QString chainId; // chain this avatar dresses — must match at load time
    AvatarType type = AvatarType::Rigid;
    std::optional<double> heightM; // overrides the chain's scale_reference

    // type == Rigid
    QMap<QString, double> segmentRadiusFrac; // per joint name + "default"
    bool hasHead = false;
    double headRadiusFrac = 0.0;
    double headElongation = 1.0; // height/width ratio, 1 = sphere
    JointStyle jointStyle = JointStyle::Blend;
    RigidBuild build = RigidBuild::Capsule; // rigid.build extension field
    SculptParams sculpt;

    // presentation.ground_shadow extension field: opacity of a soft
    // contact-shadow blob the offscreen renderer lays at z=0 under the
    // animation footprint. 0 (default) = no shadow, silhouette untouched.
    double groundShadow = 0.0;

    // type == Skinned (later worksite; parsed so the file round-trips)
    QString meshFile;

    // Simple PBR material (sRGB 0xRRGGBB).
    quint32 baseColor = 0x8FB5A8;
    quint32 accentColor = 0xE8DCC8;
    double roughness = 0.7;
    double metallic = 0.0;

    double radiusFracFor(const QString& jointName) const;
};

struct AvatarResult {
    bool ok = false;
    QString error;
    QStringList warnings;
    AvatarSpec spec;
};

AvatarResult avatarFromJson(const QJsonObject& obj);
AvatarResult loadAvatarFile(const QString& path);

// One displayable solid attached to a joint, expressed in the JOINT'S LOCAL
// frame (mm): the forward-kinematics transform of that joint places it in
// the world for any pose. `accent` selects the material accent colour (head,
// visible joint spheres).
struct AvatarPart {
    TopoDS_Shape shape;
    QString name;
    bool accent = false;
};

// The seam between "what the avatar is" and "how it is drawn/exported". A
// rigid avatar emits capsules; a future skinned avatar will emit its bind
// mesh here and weights elsewhere.
class AvatarProvider {
public:
    virtual ~AvatarProvider() = default;
    virtual std::vector<AvatarPart> partsForJoint(const Chain& chain,
                                                  int index) const = 0;
};

// Stylised rigid manikin in OCCT primitives. Build "capsule" (default):
// one capsule per segment (cylinder fused with two end spheres — the
// "blend" of joint_style), an ellipsoid head at the far end of the joint
// named "neck", and, for joint_style "sphere", a slightly oversized accent
// sphere at each joint; joint_style "none" degrades the capsules to plain
// cylinders. Build "sculpted": limbs are single revolved B-spline solids
// (rounded ends, slight swell, tapering to the child's radius so bent
// joints stay continuous), the torso is a smooth loft of ellipses
// (hip/waist/chest/shoulder), the root gets a pelvis ellipsoid, and a
// small knuckle sphere covers each articulation at any flexion angle.
class RigidAvatarProvider : public AvatarProvider {
public:
    explicit RigidAvatarProvider(AvatarSpec spec);
    std::vector<AvatarPart> partsForJoint(const Chain& chain,
                                          int index) const override;

private:
    std::vector<AvatarPart> sculptedParts(const Chain& chain,
                                          int index) const;
    void appendHead(std::vector<AvatarPart>& parts, const Joint& joint,
                    double lenMm, double scaleMm, double overlap) const;
    double distalRadiusMm(const Chain& chain, int index,
                          double rProx) const;
    AvatarSpec m_spec;
};

// Sculpt fields that NAME joints (torso/pelvis targets, flatten keys) can
// only be checked against the chain the avatar dresses — the file alone
// parses fine. Called where the pair meets (CLI, GUI); returns warnings,
// never errors: a generic chain legitimately has no "spine".
QStringList avatarChainWarnings(const AvatarSpec& spec, const Chain& chain);

// Provider factory for a validated spec (Rigid today; Skinned refuses with
// a clear message until that worksite opens).
struct ProviderResult {
    bool ok = false;
    QString error;
    std::unique_ptr<AvatarProvider> provider;
};
ProviderResult makeAvatarProvider(const AvatarSpec& spec);

} // namespace anim
} // namespace viki
