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

        // The joint's OWN motion, whatever its position in the chain.
        gp_Quaternion typedRot; // identity
        gp_Vec slide(0, 0, 0);
        switch (j.type) {
        case JointType::Ball:
            typedRot = ch.rot;
            break;
        case JointType::Revolute:
            typedRot.SetVectorAndAngle(j.axis, ch.scalar);
            break;
        case JointType::Prismatic:
            slide = j.axis * ch.scalar;
            break;
        case JointType::Fixed:
        case JointType::Free:
            break;
        }
        gp_Trsf rot;
        rot.SetRotation(typedRot);

        if (j.parent < 0) {
            // Root: absolute placement from the root channels, COMPOSED
            // with the root's own typed channel — a revolute or prismatic
            // root is the natural encoding of a lever or a robot base, and
            // dropping its value would freeze the whole mechanism.
            gp_Trsf place;
            place.SetTranslation(pose.rootPosMm);
            gp_Trsf placeRot;
            placeRot.SetRotation(pose.rootRot);
            gp_Trsf slideT;
            slideT.SetTranslation(slide);
            world[i] = place * placeRot * slideT * rot;
        } else {
            gp_Trsf trans;
            trans.SetTranslation(j.attachMm + slide);
            world[i] = world[static_cast<size_t>(j.parent)] * trans * rot;
        }
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
