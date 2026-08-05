#include "anim/ForwardKinematics.h"

namespace viki {
namespace anim {

std::vector<gp_Trsf> worldTransforms(const Chain& chain,
                                     const PoseSample& pose)
{
    std::vector<gp_Trsf> world(chain.joints.size());
    for (size_t i = 0; i < chain.joints.size(); ++i) {
        const Joint& j = chain.joints[i];
        const JointChannel& ch =
            (i < pose.values.size()) ? pose.values[i] : JointChannel{};

        gp_Vec translation = j.attachMm;
        gp_Quaternion rotation; // identity
        switch (j.type) {
        case JointType::Ball:
            rotation = ch.rot;
            break;
        case JointType::Revolute:
            rotation.SetVectorAndAngle(j.axis, ch.scalar);
            break;
        case JointType::Prismatic:
            translation += j.axis * ch.scalar;
            break;
        case JointType::Fixed:
            break;
        case JointType::Free:
            break;
        }
        if (j.parent < 0) {
            // Root: absolute placement from the root channels.
            translation = pose.rootPosMm;
            rotation = pose.rootRot;
        }

        gp_Trsf rot;
        rot.SetRotation(rotation);
        gp_Trsf trans;
        trans.SetTranslation(translation);
        gp_Trsf local = trans * rot;

        if (j.parent < 0)
            world[i] = local;
        else
            world[i] = world[static_cast<size_t>(j.parent)] * local;
    }
    return world;
}

gp_Pnt jointOrigin(const std::vector<gp_Trsf>& transforms, int index)
{
    const gp_XYZ o = transforms[static_cast<size_t>(index)]
                         .TranslationPart();
    return gp_Pnt(o);
}

gp_Pnt segmentEnd(const Chain& chain,
                  const std::vector<gp_Trsf>& transforms, int index)
{
    const Joint& j = chain.joints[static_cast<size_t>(index)];
    gp_Pnt end(j.restDirection.XYZ() * j.lengthMm);
    end.Transform(transforms[static_cast<size_t>(index)]);
    return end;
}

} // namespace anim
} // namespace viki
