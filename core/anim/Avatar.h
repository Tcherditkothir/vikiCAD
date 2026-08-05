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

// Stylised rigid manikin in OCCT primitives: one capsule per segment
// (cylinder fused with two end spheres — the "blend" of joint_style),
// an ellipsoid head at the far end of the joint named "neck", and, for
// joint_style "sphere", a slightly oversized accent sphere at each joint.
// joint_style "none" degrades the capsules to plain cylinders.
class RigidAvatarProvider : public AvatarProvider {
public:
    explicit RigidAvatarProvider(AvatarSpec spec);
    std::vector<AvatarPart> partsForJoint(const Chain& chain,
                                          int index) const override;

private:
    AvatarSpec m_spec;
};

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
